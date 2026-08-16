// Package qr encodes a short string as a QR code.
//
// Written here rather than pulled in, for two reasons. The rules say to refuse a
// dependency unless there is a reason, and this one has an unusually good way of
// being checked: quirc is already vendored for the console to *decode* QR codes
// with, so what this package produces can be handed to a completely different
// implementation, in a different language, and read back. `make check` does that
// on every run.
//
// Deliberately narrow. It encodes bytes in byte mode at error correction level M,
// picking the smallest version that fits, and does nothing else - no kanji mode,
// no ECI, no structured append. A pairing URL is short ASCII, and every feature
// not implemented is a feature that cannot be wrong.
package qr

import (
	"errors"
	"fmt"
)

// Code is a square grid of modules. true is dark.
type Code struct {
	Size    int
	Modules []bool // Size*Size, row major
}

// At reports whether the module at (x, y) is dark.
func (c *Code) At(x, y int) bool {
	if x < 0 || y < 0 || x >= c.Size || y >= c.Size {
		return false
	}
	return c.Modules[y*c.Size+x]
}

// ErrTooLong is returned when the data does not fit the largest version this
// package supports.
var ErrTooLong = errors.New("qr: data too long")

// Version 1 to 10 at error correction level M. Beyond that a phone camera is
// doing better than a 3DS one anyway, and the payload this exists for is a URL
// with a short code in it.
//
// For each version: total data codewords, EC codewords per block, and the block
// layout as (count, data codewords per block) pairs.
type versionSpec struct {
	dataWords int
	ecPerBlk  int
	blocks    []blockSpec
}

type blockSpec struct {
	count int
	words int
}

var specs = map[int]versionSpec{
	1:  {16, 10, []blockSpec{{1, 16}}},
	2:  {28, 16, []blockSpec{{1, 28}}},
	3:  {44, 26, []blockSpec{{1, 44}}},
	4:  {64, 18, []blockSpec{{2, 32}}},
	5:  {86, 24, []blockSpec{{2, 43}}},
	6:  {108, 16, []blockSpec{{4, 27}}},
	7:  {124, 18, []blockSpec{{4, 31}}},
	8:  {154, 22, []blockSpec{{2, 38}, {2, 39}}},
	9:  {182, 22, []blockSpec{{3, 36}, {2, 37}}},
	10: {216, 26, []blockSpec{{4, 43}, {1, 44}}},
}

// Alignment pattern centres per version, from the specification's table.
var alignCentres = map[int][]int{
	1:  nil,
	2:  {6, 18},
	3:  {6, 22},
	4:  {6, 26},
	5:  {6, 30},
	6:  {6, 34},
	7:  {6, 22, 38},
	8:  {6, 24, 42},
	9:  {6, 26, 46},
	10: {6, 28, 50},
}

// Encode returns the smallest level M code holding data.
func Encode(data []byte) (*Code, error) {
	for version := 1; version <= 10; version++ {
		spec := specs[version]
		if !fits(len(data), version, spec) {
			continue
		}
		return encodeAt(data, version, spec)
	}
	return nil, fmt.Errorf("%w: %d bytes", ErrTooLong, len(data))
}

// Byte mode: 4 bits of mode, then a length field, then the data.
func lengthBits(version int) int {
	if version <= 9 {
		return 8
	}
	return 16
}

func fits(n, version int, spec versionSpec) bool {
	return 4+lengthBits(version)+n*8 <= spec.dataWords*8
}

// encodeAt builds the grid for one version, trying every mask and keeping the
// one the specification's penalty rules like best.
func encodeAt(data []byte, version int, spec versionSpec) (*Code, error) {
	words := buildCodewords(data, version, spec)
	size := 17 + version*4

	var best *matrix
	bestScore := -1
	for mask := 0; mask < 8; mask++ {
		m := newMatrix(size)
		m.placePatterns(version)
		m.placeData(words)
		m.applyMask(mask)
		m.placeFormat(mask)

		score := m.penalty()
		if bestScore < 0 || score < bestScore {
			bestScore = score
			best = m
		}
	}

	return &Code{Size: best.size, Modules: best.modules}, nil
}
