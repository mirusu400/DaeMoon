package qr

// The parts of QR that are arithmetic: bitstream, Reed-Solomon, interleaving.

// bitWriter appends most significant bit first, which is the order the format
// uses everywhere.
type bitWriter struct {
	bytes []byte
	nbits int
}

func (w *bitWriter) write(value, bits int) {
	for i := bits - 1; i >= 0; i-- {
		if w.nbits%8 == 0 {
			w.bytes = append(w.bytes, 0)
		}
		if value&(1<<uint(i)) != 0 {
			w.bytes[w.nbits/8] |= 0x80 >> uint(w.nbits%8)
		}
		w.nbits++
	}
}

// GF(256) with the QR primitive polynomial 0x11d.
var (
	gfExp [512]byte
	gfLog [256]byte
)

func init() {
	x := 1
	for i := 0; i < 255; i++ {
		gfExp[i] = byte(x)
		gfLog[x] = byte(i)
		x <<= 1
		if x&0x100 != 0 {
			x ^= 0x11d
		}
	}
	for i := 255; i < 512; i++ {
		gfExp[i] = gfExp[i-255]
	}
}

func gfMul(a, b byte) byte {
	if a == 0 || b == 0 {
		return 0
	}
	return gfExp[int(gfLog[a])+int(gfLog[b])]
}

// generator returns the generator polynomial for n error correction codewords.
func generator(n int) []byte {
	poly := []byte{1}
	for i := 0; i < n; i++ {
		next := make([]byte, len(poly)+1)
		for j, c := range poly {
			next[j] ^= c
			next[j+1] ^= gfMul(c, gfExp[i])
		}
		poly = next
	}
	return poly
}

// ecCodewords returns the n error correction codewords for one block.
func ecCodewords(data []byte, n int) []byte {
	gen := generator(n)
	rem := make([]byte, len(data)+n)
	copy(rem, data)

	for i := 0; i < len(data); i++ {
		lead := rem[i]
		if lead == 0 {
			continue
		}
		for j, g := range gen {
			rem[i+j] ^= gfMul(g, lead)
		}
	}
	return rem[len(data):]
}

// The two pad bytes alternate after the terminator, which the specification
// fixes rather than leaving to the encoder.
var padBytes = [2]byte{0xec, 0x11}

// buildCodewords produces the final interleaved data + error correction stream.
func buildCodewords(data []byte, version int, spec versionSpec) []byte {
	w := &bitWriter{}
	w.write(0x4, 4) // byte mode
	w.write(len(data), lengthBits(version))
	for _, b := range data {
		w.write(int(b), 8)
	}

	// Terminator, up to four bits, then pad to a byte boundary.
	capacity := spec.dataWords * 8
	if remaining := capacity - w.nbits; remaining > 0 {
		if remaining > 4 {
			remaining = 4
		}
		w.write(0, remaining)
	}
	if pad := w.nbits % 8; pad != 0 {
		w.write(0, 8-pad)
	}

	words := w.bytes
	for i := 0; len(words) < spec.dataWords; i++ {
		words = append(words, padBytes[i%2])
	}

	// Split into blocks, compute error correction per block, then interleave both
	// groups the way the specification lays them out.
	var dataBlocks [][]byte
	var ecBlocks [][]byte
	at := 0
	for _, b := range spec.blocks {
		for i := 0; i < b.count; i++ {
			block := words[at : at+b.words]
			at += b.words
			dataBlocks = append(dataBlocks, block)
			ecBlocks = append(ecBlocks, ecCodewords(block, spec.ecPerBlk))
		}
	}

	out := make([]byte, 0, len(words)+len(ecBlocks)*spec.ecPerBlk)
	longest := 0
	for _, b := range dataBlocks {
		if len(b) > longest {
			longest = len(b)
		}
	}
	for i := 0; i < longest; i++ {
		for _, b := range dataBlocks {
			if i < len(b) {
				out = append(out, b[i])
			}
		}
	}
	for i := 0; i < spec.ecPerBlk; i++ {
		for _, b := range ecBlocks {
			out = append(out, b[i])
		}
	}
	return out
}
