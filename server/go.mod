// The Go module lives here and not at the repository root: the root has a vendor/
// directory holding copied in C sources (miniz, jsmn), and Go treats a top level
// vendor/ as its own. Keeping the module under server/ leaves that name free for
// what the C build needs it for.
module github.com/mirusu400/DaeMoon/server

go 1.25.0

require (
	github.com/dustin/go-humanize v1.0.1 // indirect
	github.com/go-chi/chi/v5 v5.3.1 // indirect
	github.com/google/uuid v1.6.0 // indirect
	github.com/mattn/go-isatty v0.0.24 // indirect
	github.com/ncruces/go-strftime v1.0.0 // indirect
	github.com/remyoudompheng/bigfft v0.0.0-20230129092748-24d4a6f8daec // indirect
	golang.org/x/sys v0.47.0 // indirect
	modernc.org/libc v1.74.4 // indirect
	modernc.org/mathutil v1.7.1 // indirect
	modernc.org/memory v1.11.0 // indirect
	modernc.org/sqlite v1.56.0 // indirect
)
