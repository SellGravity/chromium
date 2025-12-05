// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class SessionNoiseCacheBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // Enable canvas noise with a specific seed for testing
    command_line->AppendSwitchASCII("canvas-seed", "12345678");
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
  }
};

// Test that the canvas seed is properly passed via command line
IN_PROC_BROWSER_TEST_F(SessionNoiseCacheBrowserTest, CanvasSeedIsSet) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  EXPECT_TRUE(command_line->HasSwitch("canvas-seed"));
  EXPECT_EQ("12345678", command_line->GetSwitchValueASCII("canvas-seed"));
}

// Test that we can navigate to a page without crashes when noise is enabled
IN_PROC_BROWSER_TEST_F(SessionNoiseCacheBrowserTest, NoiseDoesNotCrash) {
  GURL url(embedded_test_server()->GetURL("/simple.html"));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));

  // Verify the page loaded successfully
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_TRUE(content::WaitForLoadStop(web_contents));
}

// Test that canvas operations work with noise enabled
IN_PROC_BROWSER_TEST_F(SessionNoiseCacheBrowserTest, CanvasOperationsWork) {
  GURL url(embedded_test_server()->GetURL("/simple.html"));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));

  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Execute JavaScript to test canvas operations
  const char kCanvasScript[] = R"(
    (function() {
      const canvas = document.createElement('canvas');
      canvas.width = 100;
      canvas.height = 100;
      const ctx = canvas.getContext('2d');
      ctx.fillStyle = '#FF0000';
      ctx.fillRect(0, 0, 100, 100);
      return canvas.toDataURL();
    })();
  )";

  std::string result =
      content::EvalJs(web_contents, kCanvasScript).ExtractString();

  // Verify we got a data URL back
  EXPECT_TRUE(result.find("data:image/png") == 0);
  EXPECT_GT(result.length(), 100u);
}

// Test that rect measurements work with noise
IN_PROC_BROWSER_TEST_F(SessionNoiseCacheBrowserTest, RectMeasurementsWork) {
  GURL url(embedded_test_server()->GetURL("/simple.html"));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));

  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Execute JavaScript to test getBoundingClientRect
  const char kRectScript[] = R"(
    (function() {
      const div = document.createElement('div');
      div.style.cssText = 'position:absolute;left:100px;top:50px;width:200px;height:100px';
      document.body.appendChild(div);
      const rect = div.getBoundingClientRect();
      document.body.removeChild(div);
      return {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height
      };
    })();
  )";

  auto result = content::EvalJs(web_contents, kRectScript);
  EXPECT_TRUE(result.error.empty());

  // Verify we got reasonable rect values
  // The result is a JavaScript object, so we need to access its properties
  const base::Value& dict = result.value;
  ASSERT_TRUE(dict.is_dict());
  
  std::optional<double> x = dict.GetDict().FindDouble("x");
  ASSERT_TRUE(x.has_value());
  EXPECT_GT(x.value(), 0.0);
  EXPECT_LT(x.value(), 1000.0);
}

}  // namespace
