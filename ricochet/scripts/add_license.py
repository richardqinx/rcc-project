import os
from pathlib import Path

# File extensions to process
EXTENSIONS = {'.cpp', '.h', '.hpp', '.c', '.js', '.rs'} 

# License template
LICENSE_TEMPLATE = """/*
 * Ricochet - A minimalist CLI web browser for fast, distraction-free navigation.
 * Copyright (C) 2026  Richard Qin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""

# Identifier to check if license already exists
IDENTIFIER = "Ricochet - A minimalist CLI web browser"

def add_license_to_file(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()

        if IDENTIFIER in content[:500]:
            print(f"[-] Skip: {file_path} (License already exists)")
            return
        with open(file_path, 'w', encoding='utf-8') as f:
            f.write(LICENSE_TEMPLATE + "\n" + content)
            print(f"[+] Updated: {file_path}")

    except Exception as e:
        print(f"[!] Error: Failed to process {file_path} -> {e}")

def main(target_dir):
    path = Path(target_dir)
    if not path.exists():
        print(f"Error: Path {target_dir} does not exist")
        return

    for file in path.rglob('*'):
        if file.suffix in EXTENSIONS:
            add_license_to_file(file)

if __name__ == "__main__":
    project_root = input("Please enter the project root directory (press Enter to use the current directory): ").strip() or "."
    main(project_root)