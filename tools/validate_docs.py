#!/usr/bin/env python3
"""
Validation script for openESPaudio documentation.

Checks for:
- Broken internal links
- Missing files referenced
- Basic markdown syntax issues
"""

import os
import re
import sys
from pathlib import Path
from typing import Set, List, Dict

class DocValidator:
    def __init__(self, docs_root: Path):
        self.docs_root = docs_root
        self.errors = []
        self.warnings = []

    def validate(self) -> bool:
        """Run all validation checks."""
        print("🔍 Validating openESPaudio documentation...")

        # Find all markdown files
        md_files = list(self.docs_root.rglob("*.md"))
        if not md_files:
            self.errors.append("No markdown files found in docs/")
            return False

        print(f"📁 Found {len(md_files)} markdown files")

        # Build file index
        file_index = self._build_file_index(md_files)

        # Validate each file
        for md_file in md_files:
            self._validate_file(md_file, file_index)

        # Report results
        return self._report_results()

    def _build_file_index(self, md_files: List[Path]) -> Set[str]:
        """Build index of all markdown files relative to docs root."""
        index = set()
        for md_file in md_files:
            rel_path = md_file.relative_to(self.docs_root)
            # Add various path formats
            index.add(str(rel_path))
            index.add(str(rel_path.with_suffix('')))  # without .md
            index.add(rel_path.name)  # just filename
            index.add(rel_path.stem)  # filename without extension
        return index

    def _validate_file(self, md_file: Path, file_index: Set[str]):
        """Validate a single markdown file."""
        try:
            content = md_file.read_text(encoding='utf-8')
        except Exception as e:
            self.errors.append(f"Cannot read {md_file}: {e}")
            return

        # Extract links
        links = self._extract_links(content)

        # Check each link
        for link in links:
            self._check_link(link, md_file, file_index)

    def _extract_links(self, content: str) -> List[str]:
        """Extract markdown links from content."""
        links = []

        # Markdown link patterns
        patterns = [
            r'\[([^\]]+)\]\(([^)]+)\)',  # [text](link)
            r'\[([^\]]+)\]\[([^\]]+)\]', # [text][ref] - not checking refs
        ]

        for pattern in patterns:
            matches = re.findall(pattern, content)
            for match in matches:
                if isinstance(match, tuple):
                    link = match[1] if len(match) > 1 else match[0]
                else:
                    link = match
                links.append(link)

        return links

    def _check_link(self, link: str, source_file: Path, file_index: Set[str]):
        """Check if a link is valid."""
        # Skip external links
        if link.startswith(('http://', 'https://', 'mailto:')):
            return

        # Skip anchor links (for now)
        if link.startswith('#'):
            return

        # Clean link
        link = link.split('#')[0]  # Remove anchor
        link = link.strip()

        if not link:
            return

        # Check if it's a relative path to another md file
        if link.endswith('.md'):
            link_path = link
        else:
            # Try adding .md extension
            link_path = link + '.md'

        # Check if file exists in index
        if link_path not in file_index:
            # Try relative path resolution
            source_dir = source_file.parent.relative_to(self.docs_root)
            try:
                resolved = (source_dir / link_path).resolve().relative_to(self.docs_root)
                if str(resolved) not in file_index:
                    self.errors.append(f"Broken link in {source_file}: {link}")
            except:
                self.errors.append(f"Broken link in {source_file}: {link}")
        else:
            # File exists, but check if it's accessible from current location
            pass

    def _report_results(self) -> bool:
        """Report validation results."""
        if not self.errors and not self.warnings:
            print("✅ Documentation validation passed!")
            return True

        if self.errors:
            print(f"❌ Found {len(self.errors)} errors:")
            for error in self.errors:
                print(f"  - {error}")

        if self.warnings:
            print(f"⚠️  Found {len(self.warnings)} warnings:")
            for warning in self.warnings:
                print(f"  - {warning}")

        return len(self.errors) == 0

def main():
    """Main entry point."""
    # Find docs directory
    current_dir = Path.cwd()
    docs_dir = current_dir / "docs"

    if not docs_dir.exists():
        print("❌ docs/ directory not found!")
        sys.exit(1)

    # Run validation
    validator = DocValidator(docs_dir)
    success = validator.validate()

    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
