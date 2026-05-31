#!/usr/bin/env bash
if [ ! -x "./soulc" ]; then echo "Error: ./soulc not found."; exit 1; fi
if [ -z "$1" ]; then echo "Usage: $0 <search_query>"; exit 1; fi
echo "Searching for '$1'... Please wait."
TMPFILE=$(mktemp)
./soulc search "$1" > "$TMPFILE"
if command -v fzf >/dev/null 2>&1; then
    SELECTION=$(cat "$TMPFILE" | fzf --prompt="Select file to download: ")
else
    IFS=$'\n'
    PS3="Enter the number of the file to download (or q to quit): "
    select SELECTION in $(cat "$TMPFILE"); do
        if [ "$REPLY" == "q" ]; then rm -f "$TMPFILE"; exit 1;
        elif [ -n "$SELECTION" ]; then break;
        else echo "Invalid selection."; fi
    done
fi
rm -f "$TMPFILE"
if [ -z "$SELECTION" ]; then echo "No file selected."; exit 1; fi
USER=$(echo "$SELECTION" | awk -F'\t' '{print $1}')
SIZE=$(echo "$SELECTION" | awk -F'\t' '{print $2}')
FILEPATH=$(echo "$SELECTION" | awk -F'\t' '{print $3}')
echo "Starting download for: $FILEPATH"
./soulc get "$USER" "$FILEPATH" "$SIZE"
