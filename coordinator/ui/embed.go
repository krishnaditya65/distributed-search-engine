// Package ui embeds the Seekr web console so the coordinator serves it from the single binary.
package ui

import "embed"

//go:embed *.html *.css *.js
var Files embed.FS
