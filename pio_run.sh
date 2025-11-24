#!/bin/bash
# Wrapper script for the PlatformIO Core CLI.
# This script executes the pio command from the virtual environment,
# passing along all provided arguments.

# Expand the tilde to the user's home directory
PIO_PATH="$HOME/.platformio/penv/bin/pio"

# Check if the pio executable exists
if [ ! -x "$PIO_PATH" ]; then
    echo "Error: PlatformIO executable not found at $PIO_PATH"
    echo "Please ensure PlatformIO Core is installed correctly."
    exit 1
fi

# Execute the pio command with all arguments passed to this script
"$PIO_PATH" "$@"
