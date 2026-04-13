/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifdef UNSAFE_BUFFERS_BUILD
// Safety: span constructions from AudioFloatArray::Data() and
// DOMFloat32Array::Data() raw pointers are bounded by their .size()/.length().
// All subsequent access is bounds-checked via base::span.
#pragma allow_unsafe_buffers
#endif

#include "third_party/blink/renderer/modules/webaudio/realtime_analyser.h"

#include <limits.h>

#include <algorithm>
#include <bit>
#include <complex>
#include <random>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "third_party/blink/renderer/platform/audio/audio_bus.h"
#include "third_party/blink/renderer/platform/audio/audio_utilities.h"
#include "third_party/blink/renderer/platform/audio/vector_math.h"
#include "third_party/blink/renderer/platform/privacy_budget/session_noise_cache.h"
#include "third_party/blink/renderer/platform/wtf/math_extras.h"

namespace blink {

namespace {

constexpr unsigned kDefaultFFTSize = 2048;
constexpr unsigned kInputBufferSize = RealtimeAnalyser::kMaxFFTSize * 2;

void ApplyWindow(float* p, size_t n) {
  DCHECK(IsMainThread());

  // Blackman window
  constexpr double alpha = 0.16;
  constexpr double a0 = 0.5 * (1 - alpha);
  constexpr double a1 = 0.5;
  constexpr double a2 = 0.5 * alpha;

  for (unsigned i = 0; i < n; ++i) {
    const double x = static_cast<double>(i) / static_cast<double>(n);
    const double window =
        a0 - a1 * cos(kTwoPiDouble * x) + a2 * cos(kTwoPiDouble * 2.0 * x);
    auto p_span = base::span<float>(p, n);
    p_span[i] *= static_cast<float>(window);
  }
}

// Returns x if x is finite (not NaN or infinite), otherwise returns
// default_value
float EnsureFinite(float x, float default_value) {
  return std::isfinite(x) ? x : default_value;
}

// Check if audio noise is enabled via command-line flag
bool IsAudioNoiseEnabled() {
  auto* cmd = base::CommandLine::ForCurrentProcess();
  return cmd && cmd->HasSwitch("audio-noise");
}

// Cached noise values for AnalyserNode readback methods.
// Computed once from SessionNoiseCache audio seed, reused on every call.
struct AnalyserNoiseCache {
  static constexpr size_t kMaxBins = 16384;  // Max FFT size / 2
  std::array<float, kMaxBins> frequency_noise = {};
  std::array<float, kMaxBins * 2> time_domain_noise = {};  // Max FFT size
  bool initialized = false;

  void Initialize() {
    if (initialized) return;

    uint64_t audio_seed = SessionNoiseCache::GetInstance().GetAudioNoiseSeed();
    if (audio_seed == 0) {
      audio_seed = SessionNoiseCache::GetInstance().GetSessionSeed();
    }

    // Generate deterministic frequency noise (±0.0001 dB)
    {
      std::mt19937_64 gen(audio_seed ^ 0xA1A2A3A4A5A6A7A8ULL);
      std::uniform_real_distribution<float> dis(-0.0001f, 0.0001f);
      for (size_t i = 0; i < kMaxBins; ++i) {
        frequency_noise[i] = dis(gen);
      }
    }

    // Generate deterministic time-domain noise (±1e-7)
    {
      std::mt19937_64 gen(audio_seed ^ 0xB1B2B3B4B5B6B7B8ULL);
      std::uniform_real_distribution<float> dis(-1e-7f, 1e-7f);
      for (size_t i = 0; i < kMaxBins * 2; ++i) {
        time_domain_noise[i] = dis(gen);
      }
    }

    initialized = true;
  }
};

static AnalyserNoiseCache g_analyser_noise;

}  // namespace

RealtimeAnalyser::RealtimeAnalyser(unsigned render_quantum_frames)
    : input_buffer_(kInputBufferSize),
      down_mix_bus_(AudioBus::Create(1, render_quantum_frames)),
      fft_size_(kDefaultFFTSize),
      magnitude_buffer_(kDefaultFFTSize / 2) {
  analysis_frame_ = std::make_unique<FFTFrame>(kDefaultFFTSize);
}

bool RealtimeAnalyser::SetFftSize(uint32_t size) {
  DCHECK(IsMainThread());

  // Only allow powers of two within the allowed range.
  if (size > kMaxFFTSize || size < kMinFFTSize || !std::has_single_bit(size)) {
    return false;
  }

  if (fft_size_ != size) {
    analysis_frame_ = std::make_unique<FFTFrame>(size);
    // m_magnitudeBuffer has size = fftSize / 2 because it contains floats
    // reduced from complex values in m_analysisFrame.
    magnitude_buffer_.Allocate(size / 2);
    fft_size_ = size;
  }

  return true;
}

void RealtimeAnalyser::GetFloatFrequencyData(DOMFloat32Array* destination_array,
                                             double current_time) {
  DCHECK(IsMainThread());
  DCHECK(destination_array);

  if (current_time > last_analysis_time_) {
    // Time has advanced since the last call; update the FFT data.
    last_analysis_time_ = current_time;
    DoFFTAnalysis();
  }

  const bool apply_noise = IsAudioNoiseEnabled();
  if (apply_noise) {
    g_analyser_noise.Initialize();
  }

  // Convert from linear magnitude to floating-point decibels.
  const size_t source_length = magnitude_buffer_.size();
  const size_t len = std::min(source_length, destination_array->length());
  if (len > 0) {
    const float* source = magnitude_buffer_.Data();
    float* destination = destination_array->Data();
    auto src_span = base::span<const float>(source, source_length);
    auto dst_span = base::span<float>(destination, len);

    for (unsigned i = 0; i < len; ++i) {
      const float linear_value = src_span[i];
      double db_mag = audio_utilities::LinearToDecibels(linear_value);
      if (apply_noise && i < AnalyserNoiseCache::kMaxBins) {
        db_mag += g_analyser_noise.frequency_noise[i];
      }
      dst_span[i] = static_cast<float>(db_mag);
    }
  }
}

void RealtimeAnalyser::GetByteFrequencyData(DOMUint8Array* destination_array,
                                            double current_time) {
  DCHECK(IsMainThread());
  DCHECK(destination_array);

  if (current_time > last_analysis_time_) {
    // Time has advanced since the last call; update the FFT data.
    last_analysis_time_ = current_time;
    DoFFTAnalysis();
  }

  const bool apply_noise = IsAudioNoiseEnabled();
  if (apply_noise) {
    g_analyser_noise.Initialize();
  }

  // Convert from linear magnitude to unsigned-byte decibels.
  const size_t source_length = magnitude_buffer_.size();
  const size_t len = std::min(source_length, destination_array->length());
  if (len > 0) {
    const double range_scale_factor =
        max_decibels_ == min_decibels_ ? 1.0
                                       : 1.0 / (max_decibels_ - min_decibels_);
    const double min_decibels = min_decibels_;

    const float* source = magnitude_buffer_.Data();
    unsigned char* destination = destination_array->Data();
    auto src_span = base::span<const float>(source, source_length);
    auto dst_span = base::span<unsigned char>(destination, len);

    for (unsigned i = 0; i < len; ++i) {
      const float linear_value = src_span[i];
      double db_mag = audio_utilities::LinearToDecibels(linear_value);
      if (apply_noise && i < AnalyserNoiseCache::kMaxBins) {
        db_mag += g_analyser_noise.frequency_noise[i];
      }

      // The range m_minDecibels to m_maxDecibels will be scaled to byte values
      // from 0 to UCHAR_MAX.
      const double scaled_value =
          UCHAR_MAX * (db_mag - min_decibels) * range_scale_factor;

      // Clip to valid range.
      dst_span[i] =
          static_cast<unsigned char>(ClampTo(scaled_value, 0, UCHAR_MAX));
    }
  }
}

void RealtimeAnalyser::GetFloatTimeDomainData(
    DOMFloat32Array* destination_array) {
  DCHECK(IsMainThread());
  DCHECK(destination_array);

  const bool apply_noise = IsAudioNoiseEnabled();
  if (apply_noise) {
    g_analyser_noise.Initialize();
  }

  const unsigned fft_size = FftSize();
  const size_t len =
      std::min(static_cast<size_t>(fft_size), destination_array->length());
  if (len > 0) {
    DCHECK_EQ(input_buffer_.size(), kInputBufferSize);
    DCHECK_GT(input_buffer_.size(), fft_size);

    const float* input_buffer = input_buffer_.Data();
    float* destination = destination_array->Data();
    auto in_span = base::span<const float>(input_buffer, kInputBufferSize);
    auto dst_span = base::span<float>(destination, len);

    const unsigned write_index = GetWriteIndex();

    for (unsigned i = 0; i < len; ++i) {
      // Buffer access is protected due to modulo operation.
      float value = in_span[(i + write_index - fft_size + kInputBufferSize) %
                       kInputBufferSize];

      if (apply_noise && i < AnalyserNoiseCache::kMaxBins * 2) {
        value += g_analyser_noise.time_domain_noise[i];
      }
      dst_span[i] = value;
    }
  }
}

void RealtimeAnalyser::GetByteTimeDomainData(DOMUint8Array* destination_array) {
  DCHECK(IsMainThread());
  DCHECK(destination_array);

  const bool apply_noise = IsAudioNoiseEnabled();
  if (apply_noise) {
    g_analyser_noise.Initialize();
  }

  const unsigned fft_size = FftSize();
  const size_t len =
      std::min(static_cast<size_t>(fft_size), destination_array->length());
  if (len > 0) {
    DCHECK_EQ(input_buffer_.size(), kInputBufferSize);
    DCHECK_GT(input_buffer_.size(), fft_size);

    const float* input_buffer = input_buffer_.Data();
    unsigned char* destination = destination_array->Data();
    auto in_span = base::span<const float>(input_buffer, kInputBufferSize);
    auto dst_span = base::span<unsigned char>(destination, len);

    const unsigned write_index = GetWriteIndex();

    for (unsigned i = 0; i < len; ++i) {
      // Buffer access is protected due to modulo operation.
      float value = in_span[(i + write_index - fft_size + kInputBufferSize) %
                       kInputBufferSize];

      if (apply_noise && i < AnalyserNoiseCache::kMaxBins * 2) {
        value += g_analyser_noise.time_domain_noise[i];
      }

      // Scale from nominal -1 -> +1 to unsigned byte.
      const double scaled_value = 128 * (value + 1);

      // Clip to valid range.
      dst_span[i] =
          static_cast<unsigned char>(ClampTo(scaled_value, 0, UCHAR_MAX));
    }
  }
}

void RealtimeAnalyser::WriteInput(AudioBus* bus, uint32_t frames_to_process) {
  DCHECK(bus);
  DCHECK_GT(bus->NumberOfChannels(), 0u);
  DCHECK_GE(bus->Channel(0)->length(), frames_to_process);

  unsigned write_index = GetWriteIndex();
  // FIXME : allow to work with non-FFTSize divisible chunking
  DCHECK_LT(write_index, input_buffer_.size());
  DCHECK_LE(write_index + frames_to_process, input_buffer_.size());

  // Perform real-time analysis
  auto input_span = base::span<float>(input_buffer_.Data(), input_buffer_.size());
  auto dest_span = input_span.subspan(write_index, frames_to_process);

  // Clear the bus and downmix the input according to the down mixing rules.
  // Then save the result in the m_inputBuffer at the appropriate place.
  down_mix_bus_->Zero();
  down_mix_bus_->SumFrom(*bus);
  dest_span.copy_from(base::span<const float>(
      down_mix_bus_->Channel(0)->Data(), frames_to_process));

  write_index += frames_to_process;
  if (write_index >= kInputBufferSize) {
    write_index = 0;
  }
  SetWriteIndex(write_index);
}

void RealtimeAnalyser::DoFFTAnalysis() {
  DCHECK(IsMainThread());

  // Unroll the input buffer into a temporary buffer, where we'll apply an
  // analysis window followed by an FFT.
  const uint32_t fft_size = FftSize();

  AudioFloatArray temporary_buffer(fft_size);
  float* input_buffer = input_buffer_.Data();
  float* temp_p = temporary_buffer.Data();
  auto input_span = base::span<float>(input_buffer, kInputBufferSize);
  auto temp_span = base::span<float>(temp_p, fft_size);

  // Take the previous fftSize values from the input buffer and copy into the
  // temporary buffer.
  const unsigned write_index = GetWriteIndex();
  if (write_index < fft_size) {
    size_t first_part = fft_size - write_index;
    temp_span.first(first_part).copy_from(
        input_span.subspan(write_index - fft_size + kInputBufferSize, first_part));
    temp_span.subspan(first_part, write_index).copy_from(
        input_span.first(write_index));
  } else {
    temp_span.copy_from(
        input_span.subspan(write_index - fft_size, fft_size));
  }

  // Window the input samples.
  ApplyWindow(temp_p, fft_size);

  // Do the analysis.
  analysis_frame_->DoFFT(temp_p);

  const AudioFloatArray& real = analysis_frame_->RealData();
  AudioFloatArray& imag = analysis_frame_->ImagData();

  // Blow away the packed nyquist component.
  imag[0] = 0;

  // Normalize so than an input sine wave at 0dBfs registers as 0dBfs (undo FFT
  // scaling factor).
  const double magnitude_scale = 1.0 / fft_size;

  // A value of 0 does no averaging with the previous result.  Larger values
  // produce slower, but smoother changes.
  const double k = ClampTo(smoothing_time_constant_, 0.0, 1.0);

  // Convert the analysis data from complex to magnitude and average with the
  // previous result.
  float* destination = magnitude_buffer_.Data();
  const size_t n = magnitude_buffer_.size();
  DCHECK_GE(real.size(), n);
  const float* real_p_data = real.Data();
  DCHECK_GE(imag.size(), n);
  const float* imag_p_data = imag.Data();
  auto real_span = base::span<const float>(real_p_data, n);
  auto imag_span = base::span<const float>(imag_p_data, n);
  auto mag_span = base::span<float>(destination, n);
  for (size_t i = 0; i < n; ++i) {
    std::complex<double> c(real_span[i], imag_span[i]);
    double scalar_magnitude = abs(c) * magnitude_scale;
    mag_span[i] =
        EnsureFinite(static_cast<float>(k * mag_span[i] +
                                        (1 - k) * scalar_magnitude),
                     0);
  }
}

}  // namespace blink
