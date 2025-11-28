#!/bin/bash

# Script to add MIT license copyright headers to all source files
# Usage: ./add_headers.sh

HEADER="// Copyright (c) 2025 rederyk
// Licensed under the MIT License. See LICENSE file for details.

"

# Function to add header to a file if it doesn't already have it
add_header() {
    local file="$1"
    # Check if file already has copyright header
    if ! head -5 "$file" | grep -q "Copyright.*rederyk"; then
        echo "Adding header to $file"
        # Create temp file with header + original content
        {
            echo "$HEADER"
            cat "$file"
        } > "${file}.tmp"
        mv "${file}.tmp" "$file"
    else
        echo "Header already exists in $file"
    fi
}

# Find all .cpp and .h files in src/ directory and subdirectories
# Exclude third-party files that already have their own licenses
find src/ -type f \( -name "*.cpp" -o -name "*.h" \) \
    ! -name "dr_mp3.h" \
    ! -name "es8311_reg.h" \
    -print0 | while IFS= read -r -d '' file; do
    add_header "$file"
done

echo "Header addition complete!"
