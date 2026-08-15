// Package migrations carries the schema.
//
// The files are embedded so the binary is the whole deployment: one static binary
// plus one database file, which is what self hosting has to mean here.
//
// Migrations are append only and numbered. Never edit one that has shipped.
package migrations

import "embed"

//go:embed *.sql
var FS embed.FS
