#!/usr/bin/env python3
"""
Per-Profile HTTP REST API Policy Server with Persistent Storage.

Features:
- HTTP REST API (more stable than Named Pipes)
- Each profile has its own blocklist/whitelist
- Policies saved to JSON file (persistent)
- Real-time policy updates
- Profile-specific management
- Multi-threaded concurrent request handling
- Connection pooling support
- CORS enabled for browser access

Usage:
    python test_policy_server_http.py

API Endpoints:
    POST /api/check_url      - Check if URL is blocked
    GET  /api/policy         - Get full policy for profile
    GET  /api/version        - Get policy version
    GET  /api/health         - Health check
"""

from flask import Flask, request, jsonify
from flask_cors import CORS
import json
import os
from pathlib import Path
import threading


class ProfilePolicyServer:
    def __init__(self, storage_file='policies.json'):
        self.storage_file = storage_file
        self.lock = threading.Lock()

        # Current active profile for console management
        self.current_profile = "Default"

        # Per-profile policies
        # Structure: {"profile_name": {"blocklist": {id: url}, "whitelist": {id: url}, ...}}
        self.profile_policies = {}

        # Load existing policies or create defaults
        self.load_policies()

        # Track active profiles
        self.active_profiles = set()

    def load_policies(self):
        """Load policies from JSON file or create defaults."""
        if os.path.exists(self.storage_file):
            try:
                with open(self.storage_file, 'r', encoding='utf-8') as f:
                    self.profile_policies = json.load(f)
                print(f"[Server] Loaded policies from {self.storage_file}")
                print(f"[Server] Profiles: {', '.join(self.profile_policies.keys())}")
            except Exception as e:
                print(f"[Server] ERROR loading policies: {e}")
                self._create_default_policies()
        else:
            print(f"[Server] No existing policy file, creating defaults")
            self._create_default_policies()

    def _create_default_policies(self):
        """Create default policy structure."""
        self.profile_policies = {
            "Default": {
                "blocklist": {
                    1: "facebook.com",
                    2: "twitter.com",
                    3: "*.reddit.com",
                    4: "tiktok.com"
                },
                "whitelist": {
                    1: "https://company.com"
                },
                "next_blocklist_id": 5,
                "next_whitelist_id": 2,
                "version": 1
            }
        }
        self.save_policies()

    def save_policies(self):
        """Save policies to JSON file."""
        try:
            with open(self.storage_file, 'w', encoding='utf-8') as f:
                json.dump(self.profile_policies, f, indent=2, ensure_ascii=False)
            print(f"[Server] Policies saved to {self.storage_file}")
        except Exception as e:
            print(f"[Server] ERROR saving policies: {e}")

    def get_profile_policy(self, profile):
        """Get or create policy for a profile."""
        with self.lock:
            if profile not in self.profile_policies:
                # Create new profile with empty lists
                self.profile_policies[profile] = {
                    "blocklist": {},
                    "whitelist": {},
                    "next_blocklist_id": 1,
                    "next_whitelist_id": 1,
                    "version": 1
                }
                self.save_policies()
                print(f"[Server] Created new profile: {profile}")

            return self.profile_policies[profile]

    def track_profile(self, profile):
        """Track active profile connections."""
        if profile != "Unknown" and profile not in self.active_profiles:
            self.active_profiles.add(profile)
            print(f"[Server] NEW PROFILE CONNECTED: {profile}")
            print(f"[Server] Active profiles: {', '.join(sorted(self.active_profiles))}")

    def check_url(self, url, profile="Unknown"):
        """Check if URL is blocked for a specific profile.

        Priority:
        1. Profile whitelist (highest priority)
        2. Profile blacklist
        3. Default allow
        """
        policy = self.get_profile_policy(profile)

        # Track profile
        self.track_profile(profile)

        # PRIORITY 1: Check whitelist first
        whitelist = list(policy["whitelist"].values())
        for pattern in whitelist:
            if self.match_pattern(url, pattern):
                print(f"[Server] ALLOWED (whitelist): {url} | Profile: [{profile}] | Pattern: {pattern}")
                return {
                    "allow": True,
                    "reason": f"Whitelisted: {pattern}"
                }

        # PRIORITY 2: Check blacklist
        blocklist = list(policy["blocklist"].values())
        for pattern in blocklist:
            if self.match_pattern(url, pattern):
                print(f"[Server] BLOCKED (blacklist): {url} | Profile: [{profile}] | Pattern: {pattern}")
                return {
                    "allow": False,
                    "reason": f"Blacklisted: {pattern}"
                }

        # PRIORITY 3: Default allow
        print(f"[Server] ALLOWED (default): {url} | Profile: [{profile}]")
        return {
            "allow": True,
            "reason": "Not in any list"
        }

    def get_policy(self, profile, role="viewer"):
        """Get full policy for a profile."""
        policy_data = self.get_profile_policy(profile)

        policy = {
            "Features": {
                "EnableHistory": True,
                "EnableDownloads": True
            },
            "HomepageLocation": "https://google.com",
            "URLBlocklist": list(policy_data["blocklist"].values()),
            "URLAllowlist": list(policy_data["whitelist"].values()),
            "version": policy_data["version"]
        }

        print(f"[Server] Sent policy for profile '{profile}' (version {policy_data['version']})")
        return policy

    def get_version(self, profile):
        """Get policy version for a profile."""
        policy = self.get_profile_policy(profile)
        return {"version": policy["version"]}

    @staticmethod
    def match_pattern(url, pattern):
        """Simple pattern matching."""
        if pattern == "*":
            return True

        if pattern.startswith("*."):
            domain = pattern[2:]
            return domain in url

        return pattern in url

    # ========== Profile Management (for console) ==========

    def list_profiles(self):
        """List all profiles."""
        with self.lock:
            print(f"[Console] Profiles ({len(self.profile_policies)}):")
            for profile_name, policy in self.profile_policies.items():
                marker = " *" if profile_name == self.current_profile else ""
                blocklist_count = len(policy["blocklist"])
                whitelist_count = len(policy["whitelist"])
                print(f"         - {profile_name}{marker} (B:{blocklist_count} W:{whitelist_count} v{policy['version']})")

    def switch_profile(self, profile_name):
        """Switch current profile for console management."""
        self.get_profile_policy(profile_name)
        self.current_profile = profile_name
        print(f"[Console] Switched to profile: {profile_name}")
        self.show_profile_info()

    def show_profile_info(self):
        """Show current profile info."""
        policy = self.get_profile_policy(self.current_profile)
        print(f"[Console] Profile: {self.current_profile}")
        print(f"[Console] Version: {policy['version']}")
        print(f"[Console] Blocklist: {len(policy['blocklist'])} URLs")
        print(f"[Console] Whitelist: {len(policy['whitelist'])} URLs")

    def add_url(self, url):
        """Add URL to current profile's blocklist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            # Check if exists
            for existing_url in policy["blocklist"].values():
                if existing_url == url:
                    print(f"[Console] WARNING: URL already in blocklist: {url}")
                    return False

            url_id = policy["next_blocklist_id"]
            policy["blocklist"][url_id] = url
            policy["next_blocklist_id"] += 1
            policy["version"] += 1

            self.save_policies()

        print(f"[Console] Added to blocklist [{url_id}] {url}")
        print(f"[Console] Profile '{self.current_profile}' updated to version {policy['version']}")
        return True

    def remove_url(self, identifier):
        """Remove URL from current profile's blocklist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            try:
                url_id = int(identifier)
                if url_id in policy["blocklist"]:
                    removed = policy["blocklist"].pop(url_id)
                    policy["version"] += 1
                    self.save_policies()
                    print(f"[Console] Removed [{url_id}] {removed}")
                    return True
                else:
                    print(f"[Console] ERROR: ID {url_id} not found")
                    return False
            except ValueError:
                print(f"[Console] ERROR: Invalid ID")
                return False

    def list_urls(self):
        """List current profile's blocklist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            if not policy["blocklist"]:
                print(f"[Console] Blocklist is empty for profile '{self.current_profile}'")
                return

            print(f"[Console] Blocklist for '{self.current_profile}' ({len(policy['blocklist'])} URLs) - v{policy['version']}:")
            for url_id, url in sorted(policy["blocklist"].items()):
                print(f"         [{url_id}] {url}")


# Create Flask app
app = Flask(__name__)
CORS(app)  # Enable CORS for browser access

# Create server instance
server = ProfilePolicyServer()


@app.route('/api/health', methods=['GET', 'POST'])
def health_check():
    """Health check endpoint."""
    return jsonify({
        "status": "healthy",
        "server": "Policy Server HTTP API",
        "profiles": len(server.profile_policies)
    })


@app.route('/api/check_url', methods=['POST'])
def check_url():
    """Check if URL is blocked.

    Request JSON:
        {
            "url": "https://example.com",
            "profile": "Default"
        }

    Response JSON:
        {
            "allow": true/false,
            "reason": "..."
        }
    """
    try:
        data = request.get_json()
        url = data.get('url', '')
        profile = data.get('profile', 'Unknown')

        if not url:
            return jsonify({"error": "Missing URL"}), 400

        result = server.check_url(url, profile)
        return jsonify(result)

    except Exception as e:
        print(f"[Server] ERROR in check_url: {e}")
        return jsonify({"error": str(e)}), 500


@app.route('/api/policy', methods=['GET'])
def get_policy():
    """Get full policy for a profile.

    Query params:
        profile: Profile name (default: "Default")
        role: User role (default: "viewer")

    Response JSON:
        {
            "Features": {...},
            "URLBlocklist": [...],
            "URLAllowlist": [...],
            "version": 1
        }
    """
    try:
        profile = request.args.get('profile', 'Default')
        role = request.args.get('role', 'viewer')

        result = server.get_policy(profile, role)
        return jsonify(result)

    except Exception as e:
        print(f"[Server] ERROR in get_policy: {e}")
        return jsonify({"error": str(e)}), 500


@app.route('/api/version', methods=['GET'])
def get_version():
    """Get policy version for a profile.

    Query params:
        profile: Profile name (default: "Default")

    Response JSON:
        {
            "version": 1
        }
    """
    try:
        profile = request.args.get('profile', 'Default')

        result = server.get_version(profile)
        return jsonify(result)

    except Exception as e:
        print(f"[Server] ERROR in get_version: {e}")
        return jsonify({"error": str(e)}), 500


def interactive_mode():
    """Interactive console for policy management."""
    print("\n" + "="*70)
    print("HTTP POLICY SERVER - Interactive Mode")
    print("="*70)
    print("\n  COMMANDS:")
    print("    list                     - Show blocklist")
    print("    add <url>                - Add URL to blocklist")
    print("    remove <id>              - Remove URL from blocklist")
    print("    profiles                 - List all profiles")
    print("    switch <profile>         - Switch profile")
    print("    info                     - Show profile info")
    print("    connections              - Show active connections")
    print("    quit                     - Exit")
    print("="*70 + "\n")

    server.show_profile_info()
    print()

    while True:
        try:
            cmd = input(f"[{server.current_profile}] >>> ").strip()
            if not cmd:
                continue

            cmd_lower = cmd.lower()

            if cmd_lower == "list":
                server.list_urls()

            elif cmd_lower.startswith("add "):
                url = cmd[4:].strip()
                if url:
                    server.add_url(url)
                else:
                    print("[Console] ERROR: Usage: add <url>")

            elif cmd_lower.startswith("remove "):
                identifier = cmd[7:].strip()
                if identifier:
                    server.remove_url(identifier)
                else:
                    print("[Console] ERROR: Usage: remove <id>")

            elif cmd_lower == "profiles":
                server.list_profiles()

            elif cmd_lower.startswith("switch "):
                profile_name = cmd[7:].strip()
                if profile_name:
                    server.switch_profile(profile_name)
                else:
                    print("[Console] ERROR: Usage: switch <profile>")

            elif cmd_lower == "info":
                server.show_profile_info()

            elif cmd_lower == "connections":
                if server.active_profiles:
                    print(f"[Console] Active connections ({len(server.active_profiles)}):")
                    for profile in sorted(server.active_profiles):
                        print(f"         - {profile}")
                else:
                    print("[Console] No active connections")

            elif cmd_lower in ["quit", "exit"]:
                print("[Console] Shutting down...")
                break

            else:
                print("[Console] ERROR: Unknown command")

        except KeyboardInterrupt:
            print("\n[Console] Shutting down...")
            break
        except EOFError:
            print("\n[Console] Input closed")
            break
        except Exception as e:
            print(f"[Console] ERROR: {e}")


def main():
    """Main entry point."""
    import sys

    # Server configuration
    HOST = '127.0.0.1'
    PORT = 8765

    print(f"[Server] Starting HTTP Policy Server...")
    print(f"[Server] URL: http://{HOST}:{PORT}")
    print(f"[Server] Storage: {server.storage_file}")
    print(f"[Server] Loaded {len(server.profile_policies)} profiles")
    print(f"[Server] Mode: HTTP REST API (stable, production-ready)")
    print()
    print(f"[Server] API Endpoints:")
    print(f"         POST http://{HOST}:{PORT}/api/check_url")
    print(f"         GET  http://{HOST}:{PORT}/api/policy")
    print(f"         GET  http://{HOST}:{PORT}/api/version")
    print(f"         GET  http://{HOST}:{PORT}/api/health")
    print()

    # Start Flask server in background thread
    server_thread = threading.Thread(
        target=lambda: app.run(host=HOST, port=PORT, debug=False, threaded=True),
        daemon=True
    )
    server_thread.start()

    # Run interactive console in main thread
    try:
        interactive_mode()
    except KeyboardInterrupt:
        print("\n[Main] Server stopped")

    print("[Main] Exiting...")
    sys.exit(0)


if __name__ == "__main__":
    main()
