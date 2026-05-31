# soulc

A minimalist, suckless-style C client for downloading files from the Soulseek network.
Designed to be easily scriptable and integrated into UNIX environments (e.g., shell scripts).

## Dependencies

- C compiler (gcc, clang, etc.)
- POSIX/BSD compatible `make`
- `zlib` (standard system library for decompressing search results)

## Build

Just run:

```sh
make
```

## Configuration

`soulc` uses environment variables for configuration.

**Required:**
- `SLSK_USER` - your Soulseek username
- `SLSK_PASS` - your Soulseek password

**Optional:**
- `SLSK_SERVER` - Soulseek server address (default: `server.slsknet.org`)
- `SLSK_PORT` - Soulseek server port (default: `2242`)
- `SLSK_LISTEN_PORT` - local port for incoming peer search responses (active mode)
- `SLSK_SEARCH_SECONDS` - number of seconds to collect search responses (default: `10`)

## Usage

`soulc` has two main commands: `search` and `get`.

### Search

```sh
export SLSK_USER="yourusername"
export SLSK_PASS="yourpassword"

soulc search "query"
```
It connects to the server, issues the search, and prints peer responses for a few seconds. Set `SLSK_LISTEN_PORT` to receive active incoming responses when your port is reachable.
Output format (Tab-separated values):
```
Username	Size	Filepath
```

### Get

```sh
soulc get "username" "filepath" "size_in_bytes"
```
It connects to the server to find the peer's IP and port, then connects directly to the peer to download the file to the current directory.
The exact file size must be provided (as printed by the search command).
