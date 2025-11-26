#!/usr/bin/env python3
"""
Per-Profile IPC Policy Server with Persistent Storage.

Features:
- Each profile has its own blocklist/whitelist
- Policies saved to JSON file (persistent)
- Real-time policy updates
- Profile-specific management

Usage:
    python test_policy_server_v2.py
"""

import win32pipe
import win32file
import json
import threading
import time
import os
from pathlib import Path


class ProfilePolicyServer:
    def __init__(self, pipe_name=r'\\.\pipe\chromium_policy', storage_file='policies.json'):
        self.pipe_name = pipe_name
        self.storage_file = storage_file
        self.running = True
        self.lock = threading.Lock()

        # Current active profile for console management
        self.current_profile = "Default"

        # Per-profile policies
        # Structure: {"profile_name": {"blocklist": {id: url}, "whitelist": {id: url}, ...}}
        self.profile_policies = {}

        # Load existing policies or create defaults
        self.load_policies()

        # Track active connections
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

    def handle_request(self, request_data):
        """Process incoming request and return response."""
        try:
            request = json.loads(request_data)
            action = request.get("action")
            profile = request.get("profile", "Unknown")

            # Track active profiles
            self.track_profile(profile)

            print(f"[Server] Received: {action} | Profile: [{profile}]")

            if action == "check_url":
                url = request.get("url", "")
                return self.check_url(url, profile)

            elif action == "get_policy":
                role = request.get("role", "viewer")
                return self.get_policy(profile, role)

            elif action == "get_policy_version":
                policy = self.get_profile_policy(profile)
                return {"version": policy["version"]}

            else:
                return {"error": "Unknown action"}

        except json.JSONDecodeError:
            return {"error": "Invalid JSON"}
        except Exception as e:
            return {"error": str(e)}

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

    @staticmethod
    def match_pattern(url, pattern):
        """Simple pattern matching."""
        if pattern == "*":
            return True

        if pattern.startswith("*."):
            domain = pattern[2:]
            return domain in url

        return pattern in url

    def serve_forever(self):
        """Main server loop."""
        print(f"[Server] Starting Per-Profile Policy Server...")
        print(f"[Server] Pipe: {self.pipe_name}")
        print(f"[Server] Storage: {self.storage_file}")
        print(f"[Server] Loaded {len(self.profile_policies)} profiles")
        print(f"[Server] Waiting for connections...")
        print()

        while self.running:
            try:
                # Create named pipe
                handle = win32pipe.CreateNamedPipe(
                    self.pipe_name,
                    win32pipe.PIPE_ACCESS_DUPLEX,
                    win32pipe.PIPE_TYPE_MESSAGE | win32pipe.PIPE_READMODE_MESSAGE | win32pipe.PIPE_WAIT,
                    win32pipe.PIPE_UNLIMITED_INSTANCES,
                    65536, 65536, 0, None)

                # Wait for client connection
                win32pipe.ConnectNamedPipe(handle, None)

                # Read request
                result, data = win32file.ReadFile(handle, 65536)
                request_data = data.decode('utf-8')

                # Handle request
                response = self.handle_request(request_data)
                response_json = json.dumps(response)

                # Send response
                win32file.WriteFile(handle, response_json.encode('utf-8'))

                # Close connection
                win32file.CloseHandle(handle)

            except Exception as e:
                if self.running:
                    print(f"[Server] ERROR: {e}")
                time.sleep(0.1)

    # ========== Profile Management ==========

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
        # Get or create profile
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

    def delete_profile(self, profile_name):
        """Delete a profile."""
        with self.lock:
            if profile_name == "Default":
                print(f"[Console] ERROR: Cannot delete Default profile")
                return False

            if profile_name not in self.profile_policies:
                print(f"[Console] ERROR: Profile '{profile_name}' not found")
                return False

            del self.profile_policies[profile_name]
            self.save_policies()

            if self.current_profile == profile_name:
                self.current_profile = "Default"

            print(f"[Console] Deleted profile: {profile_name}")
            return True

    # ========== Blocklist Management (Current Profile) ==========

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

    def clear_urls(self):
        """Clear current profile's blocklist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            count = len(policy["blocklist"])
            policy["blocklist"].clear()
            policy["version"] += 1
            self.save_policies()

        print(f"[Console] Cleared {count} URLs from profile '{self.current_profile}'")

    # ========== Whitelist Management (Current Profile) ==========

    def add_whitelist(self, url):
        """Add URL to current profile's whitelist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            # Check if exists
            for existing_url in policy["whitelist"].values():
                if existing_url == url:
                    print(f"[Console] WARNING: URL already in whitelist: {url}")
                    return False

            url_id = policy["next_whitelist_id"]
            policy["whitelist"][url_id] = url
            policy["next_whitelist_id"] += 1
            policy["version"] += 1

            self.save_policies()

        print(f"[Console] Added to whitelist [W{url_id}] {url}")
        print(f"[Console] Profile '{self.current_profile}' updated to version {policy['version']}")
        return True

    def remove_whitelist(self, identifier):
        """Remove URL from current profile's whitelist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            try:
                url_id = int(identifier)
                if url_id in policy["whitelist"]:
                    removed = policy["whitelist"].pop(url_id)
                    policy["version"] += 1
                    self.save_policies()
                    print(f"[Console] Removed from whitelist [W{url_id}] {removed}")
                    return True
                else:
                    print(f"[Console] ERROR: Whitelist ID {url_id} not found")
                    return False
            except ValueError:
                print(f"[Console] ERROR: Invalid ID")
                return False

    def list_whitelist(self):
        """List current profile's whitelist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            if not policy["whitelist"]:
                print(f"[Console] Whitelist is empty for profile '{self.current_profile}'")
                return

            print(f"[Console] Whitelist for '{self.current_profile}' ({len(policy['whitelist'])} URLs) - v{policy['version']}:")
            for url_id, url in sorted(policy["whitelist"].items()):
                print(f"         [W{url_id}] {url}")

    def clear_whitelist(self):
        """Clear current profile's whitelist."""
        policy = self.get_profile_policy(self.current_profile)

        with self.lock:
            count = len(policy["whitelist"])
            policy["whitelist"].clear()
            policy["version"] += 1
            self.save_policies()

        print(f"[Console] Cleared {count} URLs from whitelist for profile '{self.current_profile}'")


def interactive_mode(server):
    """Interactive console for per-profile policy management."""
    print("\n" + "="*70)
    print("PER-PROFILE POLICY SERVER - Interactive Mode")
    print("="*70)
    print("\n  PROFILE MANAGEMENT:")
    print("    profiles                 - List all profiles")
    print("    switch <profile>         - Switch to profile (creates if not exists)")
    print("    info                     - Show current profile info")
    print("    delete <profile>         - Delete a profile")
    print("\n  BLOCKLIST (for current profile):")
    print("    list                     - Show blocklist")
    print("    add <url>                - Add URL to blocklist")
    print("    remove <id>              - Remove URL from blocklist")
    print("    clear                    - Clear blocklist")
    print("\n  WHITELIST (for current profile):")
    print("    list-whitelist           - Show whitelist")
    print("    add-whitelist <url>      - Add URL to whitelist")
    print("    remove-whitelist <id>    - Remove URL from whitelist")
    print("    clear-whitelist          - Clear whitelist")
    print("\n  GENERAL:")
    print("    status                   - Show current profile status")
    print("    connections              - Show active connections")
    print("    quit                     - Exit")
    print("="*70)
    print("\nExamples:")
    print("  >>> profiles")
    print("  >>> switch Profile-1")
    print("  >>> add facebook.com")
    print("  >>> add-whitelist google.com")
    print("  >>> switch Default")
    print("  >>> list")
    print("="*70 + "\n")

    # Show initial profile
    server.show_profile_info()
    print()

    while server.running:
        try:
            cmd = input(f"[{server.current_profile}] >>> ").strip()
            if not cmd:
                continue

            cmd_lower = cmd.lower()

            # Profile commands
            if cmd_lower == "profiles":
                server.list_profiles()

            elif cmd_lower.startswith("switch "):
                profile_name = cmd[7:].strip()
                if profile_name:
                    server.switch_profile(profile_name)
                else:
                    print("[Console] ERROR: Usage: switch <profile>")

            elif cmd_lower == "info":
                server.show_profile_info()

            elif cmd_lower.startswith("delete "):
                profile_name = cmd[7:].strip()
                if profile_name:
                    confirm = input(f"[Console] WARNING: Delete profile '{profile_name}'? (yes/no): ").strip().lower()
                    if confirm in ["yes", "y"]:
                        server.delete_profile(profile_name)
                else:
                    print("[Console] ERROR: Usage: delete <profile>")

            # Blocklist commands
            elif cmd_lower == "list":
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

            elif cmd_lower == "clear":
                confirm = input("[Console] WARNING: Clear all URLs? (yes/no): ").strip().lower()
                if confirm in ["yes", "y"]:
                    server.clear_urls()

            # Whitelist commands
            elif cmd_lower == "list-whitelist":
                server.list_whitelist()

            elif cmd_lower.startswith("add-whitelist "):
                url = cmd[14:].strip()
                if url:
                    server.add_whitelist(url)
                else:
                    print("[Console] ERROR: Usage: add-whitelist <url>")

            elif cmd_lower.startswith("remove-whitelist "):
                identifier = cmd[17:].strip()
                if identifier:
                    server.remove_whitelist(identifier)
                else:
                    print("[Console] ERROR: Usage: remove-whitelist <id>")

            elif cmd_lower == "clear-whitelist":
                confirm = input("[Console] WARNING: Clear whitelist? (yes/no): ").strip().lower()
                if confirm in ["yes", "y"]:
                    server.clear_whitelist()

            # General commands
            elif cmd_lower == "status":
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
                server.running = False
                break

            else:
                print("[Console] ERROR: Unknown command. See help above.")

        except KeyboardInterrupt:
            print("\n[Console] Shutting down...")
            server.running = False
            break
        except EOFError:
            print("\n[Console] Input closed, shutting down...")
            server.running = False
            break
        except Exception as e:
            print(f"[Console] ERROR: {e}")


def main():
    """Main entry point."""
    server = ProfilePolicyServer()

    # Start server in background thread
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    # Run interactive mode in main thread
    interactive_mode(server)

    print("[Main] Server stopped")


if __name__ == "__main__":
    main()
