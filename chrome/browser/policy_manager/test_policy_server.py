#!/usr/bin/env python3
"""
Simple IPC Policy Server for testing PolicyIPCClient integration.

Usage:
    python test_policy_server.py

Then in another terminal:
    set CHROMIUM_POLICY_PIPE=\\\\.\\pipe\\chromium_policy
    out\\Default\\chrome.exe
"""

import win32pipe
import win32file
import json
import threading
import time


class TestPolicyServer:
    def __init__(self, pipe_name=r'\\.\pipe\chromium_policy'):
        self.pipe_name = pipe_name
        self.policy_version = 1
        self.running = True
        self.active_profiles = set()  # Track active profiles
        self.next_blocklist_id = 5  # Start from 5 (IDs 1-4 taken by defaults)
        self.next_whitelist_id = 2  # Start from 2 (ID 1 taken by default)
        self.lock = threading.Lock()  # Thread-safe policy updates

        # Blocklist with IDs
        self.url_blocklist = {
            1: "facebook.com",
            2: "twitter.com",
            3: "*.reddit.com",
            4: "tiktok.com"
        }

        # Whitelist with IDs
        self.url_whitelist = {
            1: "https://company.com"
        }

        self.policies = {
            "viewer": {
                "Features": {
                    "EnableHistory": False,
                    "EnableDownloads": False
                },
                "HomepageLocation": "https://google.com"
            }
        }

    def get_blocklist_array(self):
        """Convert blocklist dict to array for policy response."""
        with self.lock:
            return list(self.url_blocklist.values())

    def get_whitelist_array(self):
        """Convert whitelist dict to array for policy response."""
        with self.lock:
            return list(self.url_whitelist.values())

    def handle_request(self, request_data):
        """Process incoming request and return response."""
        try:
            request = json.loads(request_data)
            action = request.get("action")
            profile = request.get("profile", "Unknown")

            # Track active profiles from any request
            self.track_profile(profile)

            print(f"[Server] Received: {action} | Profile: [{profile}]")

            if action == "check_url":
                url = request.get("url", "")
                return self.check_url(url, profile)

            elif action == "get_policy":
                role = request.get("role", "viewer")
                return self.get_policy(role)

            elif action == "get_policy_version":
                return {"version": self.policy_version}

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
        """Check if URL is blocked with whitelist/blacklist priority logic.

        Priority:
        1. Whitelist check (highest priority - overrides blacklist)
        2. Blacklist check
        3. Default allow (if not in any list)
        """
        # PRIORITY 1: Check WHITELIST first (whitelist overrides everything)
        whitelist = self.get_whitelist_array()
        for pattern in whitelist:
            if self.match_pattern(url, pattern):
                print(f"[Server] ALLOWED (whitelist): {url} | Profile: [{profile}] | Pattern: {pattern}")
                return {
                    "allow": True,
                    "reason": f"Whitelisted: {pattern}"
                }

        # PRIORITY 2: Check BLACKLIST
        blocklist = self.get_blocklist_array()
        for pattern in blocklist:
            if self.match_pattern(url, pattern):
                print(f"[Server] BLOCKED (blacklist): {url} | Profile: [{profile}] | Pattern: {pattern}")
                return {
                    "allow": False,
                    "reason": f"Blacklisted: {pattern}"
                }

        # PRIORITY 3: Default ALLOW (not in any list)
        print(f"[Server] ALLOWED (default): {url} | Profile: [{profile}]")
        return {
            "allow": True,
            "reason": "Not in any list"
        }

    def get_policy(self, role):
        """Get full policy for a role."""
        policy = self.policies.get(role, self.policies["viewer"]).copy()
        policy["URLBlocklist"] = self.get_blocklist_array()
        policy["URLAllowlist"] = self.get_whitelist_array()
        policy["version"] = self.policy_version
        print(f"[Server] Sent policy for role '{role}' (version {self.policy_version})")
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
        print(f"[Server] Starting policy server...")
        print(f"[Server] Pipe: {self.pipe_name}")
        print(f"[Server] Policy version: {self.policy_version}")
        print(f"[Server] Blocklist ({len(self.url_blocklist)} URLs):")
        for url_id, url in sorted(self.url_blocklist.items()):
            print(f"         [{url_id}] {url}")
        print(f"[Server] Whitelist ({len(self.url_whitelist)} URLs):")
        for url_id, url in sorted(self.url_whitelist.items()):
            print(f"         [W{url_id}] {url}")
        print(f"[Server] Waiting for connections...")
        print()

        while self.running:
            try:
                # Create named pipe (allow multiple instances for concurrent requests)
                handle = win32pipe.CreateNamedPipe(
                    self.pipe_name,
                    win32pipe.PIPE_ACCESS_DUPLEX,
                    win32pipe.PIPE_TYPE_BYTE |
                    win32pipe.PIPE_READMODE_BYTE |
                    win32pipe.PIPE_WAIT,
                    win32pipe.PIPE_UNLIMITED_INSTANCES,  # Allow unlimited connections
                    4096,   # Out buffer size
                    4096,   # In buffer size
                    0,      # Default timeout
                    None    # Security attributes
                )

                # Wait for client connection
                win32pipe.ConnectNamedPipe(handle)

                # Read request
                result = win32file.ReadFile(handle, 4096)
                request_data = result[1].decode('utf-8')

                # Process request
                response = self.handle_request(request_data)
                response_json = json.dumps(response)

                # Send response
                win32file.WriteFile(handle, response_json.encode('utf-8'))

                # Close connection
                win32file.CloseHandle(handle)

            except Exception as e:
                if self.running:  # Only print errors if still running
                    print(f"[Server] ERROR: {e}")
                time.sleep(0.1)

    def add_url(self, url):
        """Add new URL to blocklist."""
        with self.lock:
            # Check if URL already exists
            for existing_url in self.url_blocklist.values():
                if existing_url == url:
                    print(f"[Console] WARNING: URL already exists in blocklist: {url}")
                    return False

            url_id = self.next_blocklist_id
            self.url_blocklist[url_id] = url
            self.next_blocklist_id += 1
            self.policy_version += 1

        print(f"[Console] Added to blocklist [{url_id}] {url}")
        print(f"[Console] Policy updated to version {self.policy_version}")
        return True

    def remove_url(self, identifier):
        """Remove URL by ID or pattern."""
        with self.lock:
            # Try to parse as ID first
            try:
                url_id = int(identifier)
                if url_id in self.url_blocklist:
                    removed = self.url_blocklist.pop(url_id)
                    self.policy_version += 1
                    print(f"[Console] Removed [{url_id}] {removed}")
                    print(f"[Console] Policy updated to version {self.policy_version}")
                    return True
                else:
                    print(f"[Console] ERROR: ID {url_id} not found")
                    return False
            except ValueError:
                # Not an ID, try to match URL pattern
                found_ids = []
                for url_id, url in self.url_blocklist.items():
                    if url == identifier or identifier in url:
                        found_ids.append(url_id)

                if not found_ids:
                    print(f"[Console] ERROR: URL pattern '{identifier}' not found")
                    return False

                if len(found_ids) == 1:
                    url_id = found_ids[0]
                    removed = self.url_blocklist.pop(url_id)
                    self.policy_version += 1
                    print(f"[Console] Removed [{url_id}] {removed}")
                    print(f"[Console] Policy updated to version {self.policy_version}")
                    return True
                else:
                    print(f"[Console] ERROR: Multiple matches found. Please use ID:")
                    for url_id in found_ids:
                        print(f"         [{url_id}] {self.url_blocklist[url_id]}")
                    return False

    def list_urls(self):
        """List all URLs with IDs."""
        with self.lock:
            if not self.url_blocklist:
                print("[Console] Blocklist is empty")
                return

            print(f"[Console] Blocklist ({len(self.url_blocklist)} URLs) - Policy v{self.policy_version}:")
            for url_id, url in sorted(self.url_blocklist.items()):
                print(f"         [{url_id}] {url}")

    def clear_urls(self):
        """Clear all URLs from blocklist."""
        with self.lock:
            count = len(self.url_blocklist)
            self.url_blocklist.clear()
            self.policy_version += 1

        print(f"[Console] Cleared {count} URLs")
        print(f"[Console] Policy updated to version {self.policy_version}")

    def replace_blocklist(self, new_urls):
        """Replace entire blocklist (legacy command)."""
        with self.lock:
            self.url_blocklist.clear()
            self.next_blocklist_id = 1
            for url in new_urls:
                self.url_blocklist[self.next_blocklist_id] = url
                self.next_blocklist_id += 1
            self.policy_version += 1

        print(f"[Console] Blocklist replaced with {len(new_urls)} URLs")
        print(f"[Console] Policy updated to version {self.policy_version}")

    # ========== Whitelist Management Methods ==========

    def add_whitelist(self, url):
        """Add URL to whitelist."""
        with self.lock:
            # Check if URL already exists
            for existing_url in self.url_whitelist.values():
                if existing_url == url:
                    print(f"[Console] WARNING: URL already exists in whitelist: {url}")
                    return False

            url_id = self.next_whitelist_id
            self.url_whitelist[url_id] = url
            self.next_whitelist_id += 1
            self.policy_version += 1

        print(f"[Console] Added to whitelist [W{url_id}] {url}")
        print(f"[Console] Policy updated to version {self.policy_version}")
        return True

    def remove_whitelist(self, identifier):
        """Remove URL from whitelist by ID or pattern."""
        with self.lock:
            # Try to parse as ID first
            try:
                url_id = int(identifier)
                if url_id in self.url_whitelist:
                    removed = self.url_whitelist.pop(url_id)
                    self.policy_version += 1
                    print(f"[Console] Removed from whitelist [W{url_id}] {removed}")
                    print(f"[Console] Policy updated to version {self.policy_version}")
                    return True
                else:
                    print(f"[Console] ERROR: Whitelist ID {url_id} not found")
                    return False
            except ValueError:
                # Not an ID, try to match URL pattern
                found_ids = []
                for url_id, url in self.url_whitelist.items():
                    if url == identifier or identifier in url:
                        found_ids.append(url_id)

                if not found_ids:
                    print(f"[Console] ERROR: URL pattern '{identifier}' not found in whitelist")
                    return False

                if len(found_ids) == 1:
                    url_id = found_ids[0]
                    removed = self.url_whitelist.pop(url_id)
                    self.policy_version += 1
                    print(f"[Console] Removed from whitelist [W{url_id}] {removed}")
                    print(f"[Console] Policy updated to version {self.policy_version}")
                    return True
                else:
                    print(f"[Console] ERROR: Multiple matches found in whitelist. Please use ID:")
                    for url_id in found_ids:
                        print(f"         [W{url_id}] {self.url_whitelist[url_id]}")
                    return False

    def list_whitelist(self):
        """List all whitelisted URLs with IDs."""
        with self.lock:
            if not self.url_whitelist:
                print("[Console] Whitelist is empty")
                return

            print(f"[Console] Whitelist ({len(self.url_whitelist)} URLs) - Policy v{self.policy_version}:")
            for url_id, url in sorted(self.url_whitelist.items()):
                print(f"         [W{url_id}] {url}")

    def clear_whitelist(self):
        """Clear all URLs from whitelist."""
        with self.lock:
            count = len(self.url_whitelist)
            self.url_whitelist.clear()
            self.policy_version += 1

        print(f"[Console] Cleared {count} URLs from whitelist")
        print(f"[Console] Policy updated to version {self.policy_version}")


def interactive_mode(server):
    """Interactive console for updating policies."""
    print("\n" + "="*70)
    print("INTERACTIVE MODE - Commands:")
    print("\n  BLOCKLIST:")
    print("    list                     - Show all blocked URLs with IDs")
    print("    add <url>                - Add new URL to blocklist")
    print("    remove <id or url>       - Remove URL by ID or pattern")
    print("    clear                    - Clear all blocked URLs")
    print("    block <url1,url2,...>    - Replace entire blocklist")
    print("\n  WHITELIST:")
    print("    list-whitelist           - Show all whitelisted URLs with IDs")
    print("    add-whitelist <url>      - Add new URL to whitelist")
    print("    remove-whitelist <id>    - Remove URL from whitelist by ID")
    print("    clear-whitelist          - Clear all whitelisted URLs")
    print("\n  GENERAL:")
    print("    version                  - Show policy version")
    print("    status                   - Show current policy")
    print("    profiles                 - Show active profiles")
    print("    quit                     - Exit")
    print("="*70)
    print("\nExamples:")
    print("  >>> list")
    print("  >>> add youtube.com")
    print("  >>> add *.instagram.com")
    print("  >>> remove 5")
    print("  >>> list-whitelist")
    print("  >>> add-whitelist google.com")
    print("  >>> add-whitelist *.company.com")
    print("  >>> remove-whitelist 2")
    print("="*70 + "\n")

    while server.running:
        try:
            cmd = input(">>> ").strip()
            if not cmd:
                continue

            cmd_lower = cmd.lower()

            if cmd_lower == "quit":
                print("[Console] Shutting down...")
                server.running = False
                break

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
                    print("[Console] ERROR: Usage: remove <id or url>")

            elif cmd_lower == "clear":
                confirm = input("[Console] WARNING: Clear all URLs? (yes/no): ").strip().lower()
                if confirm in ["yes", "y"]:
                    server.clear_urls()
                else:
                    print("[Console] Cancelled")

            elif cmd_lower == "version":
                print(f"[Console] Policy version: {server.policy_version}")

            elif cmd_lower == "status":
                policy = server.get_policy("viewer")
                print(f"[Console] Current policy (v{server.policy_version}):")
                print(json.dumps(policy, indent=2))

            elif cmd_lower == "profiles":
                if server.active_profiles:
                    print(f"[Console] Active profiles ({len(server.active_profiles)}):")
                    for profile in sorted(server.active_profiles):
                        print(f"         - {profile}")
                else:
                    print("[Console] No active profiles connected yet")

            elif cmd_lower.startswith("block "):
                sites = cmd[6:].split(",")
                sites = [s.strip() for s in sites if s.strip()]
                if sites:
                    server.replace_blocklist(sites)
                else:
                    print("[Console] ERROR: Usage: block <url1,url2,...>")

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
                    print("[Console] ERROR: Usage: remove-whitelist <id or url>")

            elif cmd_lower == "clear-whitelist":
                confirm = input("[Console] WARNING: Clear all whitelisted URLs? (yes/no): ").strip().lower()
                if confirm in ["yes", "y"]:
                    server.clear_whitelist()
                else:
                    print("[Console] Cancelled")

            else:
                print("[Console] ERROR: Unknown command. Type a command or see examples above.")

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
    server = TestPolicyServer()

    # Start server in background thread
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    # Give server time to start
    time.sleep(0.5)

    # Run interactive console in main thread
    try:
        interactive_mode(server)
    except KeyboardInterrupt:
        print("\n[Main] Interrupted")
    finally:
        server.running = False
        print("[Main] Server stopped")


if __name__ == "__main__":
    main()
