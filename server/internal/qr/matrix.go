package qr

// Laying the codewords out on the grid: fixed patterns, the zigzag, masking, and
// the format information.

type matrix struct {
	size     int
	modules  []bool
	reserved []bool // fixed patterns, which data skips and masking must not touch
}

func newMatrix(size int) *matrix {
	return &matrix{
		size:     size,
		modules:  make([]bool, size*size),
		reserved: make([]bool, size*size),
	}
}

func (m *matrix) set(x, y int, dark, fixed bool) {
	if x < 0 || y < 0 || x >= m.size || y >= m.size {
		return
	}
	m.modules[y*m.size+x] = dark
	if fixed {
		m.reserved[y*m.size+x] = true
	}
}

func (m *matrix) get(x, y int) bool {
	if x < 0 || y < 0 || x >= m.size || y >= m.size {
		return false
	}
	return m.modules[y*m.size+x]
}

func (m *matrix) isFixed(x, y int) bool { return m.reserved[y*m.size+x] }

// A finder pattern plus the light separator that has to surround it.
func (m *matrix) placeFinder(ox, oy int) {
	for dy := -1; dy <= 7; dy++ {
		for dx := -1; dx <= 7; dx++ {
			x, y := ox+dx, oy+dy
			if x < 0 || y < 0 || x >= m.size || y >= m.size {
				continue
			}
			inner := dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6
			dark := false
			if inner {
				edge := dx == 0 || dx == 6 || dy == 0 || dy == 6
				centre := dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4
				dark = edge || centre
			}
			m.set(x, y, dark, true)
		}
	}
}

func (m *matrix) placeAlignment(cx, cy int) {
	for dy := -2; dy <= 2; dy++ {
		for dx := -2; dx <= 2; dx++ {
			ring := dx == -2 || dx == 2 || dy == -2 || dy == 2
			m.set(cx+dx, cy+dy, ring || (dx == 0 && dy == 0), true)
		}
	}
}

func (m *matrix) placePatterns(version int) {
	m.placeFinder(0, 0)
	m.placeFinder(m.size-7, 0)
	m.placeFinder(0, m.size-7)

	// Timing patterns.
	for i := 8; i < m.size-8; i++ {
		dark := i%2 == 0
		m.set(i, 6, dark, true)
		m.set(6, i, dark, true)
	}

	// Alignment patterns, at every pairing of centres except where one would sit
	// on top of a finder.
	centres := alignCentres[version]
	for _, cy := range centres {
		for _, cx := range centres {
			if (cx == 6 && cy == 6) ||
				(cx == 6 && cy == m.size-7) ||
				(cx == m.size-7 && cy == 6) {
				continue
			}
			m.placeAlignment(cx, cy)
		}
	}

	// Reserve the format information areas. What goes in them is not known until
	// the mask is chosen, but the data must not be written here.
	for i := 0; i < 9; i++ {
		if i != 6 {
			m.set(8, i, false, true)
			m.set(i, 8, false, true)
		}
	}
	for i := 0; i < 8; i++ {
		m.set(8, m.size-1-i, false, true)
		m.set(m.size-1-i, 8, false, true)
	}

	// The dark module. Always set, always here, and easy to forget: without it a
	// decoder reads the format information and gives up.
	m.set(8, m.size-8, true, true)

	// Version 7 and up carry the version number twice, in two 6x3 blocks tucked
	// against the top right and bottom left finders.
	//
	// Leaving this out does not produce a code that is merely missing a field: the
	// thirty six modules it occupies are then handed to the data, everything after
	// them shifts, and the payload turns to noise a third of the way in. Versions
	// 1 to 6 have no such block, which is why every smaller code decoded and the
	// first one over the line did not.
	if version >= 7 {
		bits := versionBits(version)

		for i := 0; i < 18; i++ {
			dark := bits&(1<<uint(i)) != 0
			a, b := i/3, i%3

			m.set(a, m.size-11+b, dark, true)
			m.set(m.size-11+b, a, dark, true)
		}
	}
}

// versionBits is the version number with its BCH(18,6) check bits.
func versionBits(version int) int {
	rem := version << 12
	for i := 5; i >= 0; i-- {
		if rem&(1<<uint(i+12)) != 0 {
			rem ^= 0x1f25 << uint(i)
		}
	}
	return version<<12 | rem
}

// placeData walks two-module-wide columns from the bottom right, upward then
// downward, skipping the vertical timing pattern and anything already fixed.
func (m *matrix) placeData(words []byte) {
	bit := 0
	upward := true

	for right := m.size - 1; right >= 1; right -= 2 {
		if right == 6 {
			right = 5 // the timing column is not half of a column pair
		}
		for i := 0; i < m.size; i++ {
			y := i
			if upward {
				y = m.size - 1 - i
			}
			for dx := 0; dx < 2; dx++ {
				x := right - dx
				if m.isFixed(x, y) {
					continue
				}
				dark := false
				if bit < len(words)*8 {
					dark = words[bit/8]&(0x80>>uint(bit%8)) != 0
				}
				m.set(x, y, dark, false)
				bit++
			}
		}
		upward = !upward
	}
}

func maskAt(mask, x, y int) bool {
	switch mask {
	case 0:
		return (x+y)%2 == 0
	case 1:
		return y%2 == 0
	case 2:
		return x%3 == 0
	case 3:
		return (x+y)%3 == 0
	case 4:
		return (y/2+x/3)%2 == 0
	case 5:
		return (x*y)%2+(x*y)%3 == 0
	case 6:
		return ((x*y)%2+(x*y)%3)%2 == 0
	default:
		return ((x+y)%2+(x*y)%3)%2 == 0
	}
}

func (m *matrix) applyMask(mask int) {
	for y := 0; y < m.size; y++ {
		for x := 0; x < m.size; x++ {
			if m.isFixed(x, y) {
				continue
			}
			if maskAt(mask, x, y) {
				m.modules[y*m.size+x] = !m.modules[y*m.size+x]
			}
		}
	}
}

// Format information: two bits of level, three of mask, BCH(15,5), then a fixed
// XOR so an all-zero format is not all-zero on the grid.
func formatBits(mask int) int {
	const levelM = 0 // the two bit code for error correction level M
	data := levelM<<3 | mask
	rem := data << 10
	for i := 4; i >= 0; i-- {
		if rem&(1<<uint(i+10)) != 0 {
			rem ^= 0x537 << uint(i)
		}
	}
	return (data<<10 | rem) ^ 0x5412
}

// The format information is written twice, in two differently shaped runs, so
// that losing one finder does not lose it. Both copies are laid out here.
func (m *matrix) placeFormat(mask int) {
	bits := formatBits(mask)

	for i := 0; i < 15; i++ {
		dark := bits&(1<<uint(i)) != 0

		// Around the top left finder: up the left column, then along the top row.
		switch {
		case i < 6:
			m.modules[i*m.size+8] = dark
		case i == 6:
			m.modules[7*m.size+8] = dark
		case i == 7:
			m.modules[8*m.size+8] = dark
		case i == 8:
			m.modules[8*m.size+7] = dark
		default:
			m.modules[8*m.size+(14-i)] = dark
		}

		// And split between the other two finders.
		if i < 8 {
			m.modules[8*m.size+(m.size-1-i)] = dark
		} else {
			m.modules[(m.size-15+i)*m.size+8] = dark
		}
	}
}

// penalty scores a masked grid by the specification's four rules. Lower is
// better. Any mask decodes; this picks the one a camera has the easiest time
// with, which on a 3DS is worth the sixty lines.
func (m *matrix) penalty() int {
	score := 0

	// Rule 1: runs of five or more of the same colour, in both directions.
	for _, byRow := range []bool{true, false} {
		for a := 0; a < m.size; a++ {
			run := 1
			for b := 1; b < m.size; b++ {
				var cur, prev bool
				if byRow {
					cur, prev = m.get(b, a), m.get(b-1, a)
				} else {
					cur, prev = m.get(a, b), m.get(a, b-1)
				}
				if cur == prev {
					run++
					continue
				}
				if run >= 5 {
					score += 3 + (run - 5)
				}
				run = 1
			}
			if run >= 5 {
				score += 3 + (run - 5)
			}
		}
	}

	// Rule 2: every 2x2 block of one colour.
	for y := 0; y+1 < m.size; y++ {
		for x := 0; x+1 < m.size; x++ {
			c := m.get(x, y)
			if c == m.get(x+1, y) && c == m.get(x, y+1) && c == m.get(x+1, y+1) {
				score += 3
			}
		}
	}

	// Rule 3: the finder-like 1:1:3:1:1 sequence with four light modules on
	// either side, which is what confuses a decoder into seeing a finder.
	pattern := []bool{true, false, true, true, true, false, true}
	quiet := []bool{false, false, false, false}
	for _, byRow := range []bool{true, false} {
		for a := 0; a < m.size; a++ {
			line := make([]bool, m.size)
			for b := 0; b < m.size; b++ {
				if byRow {
					line[b] = m.get(b, a)
				} else {
					line[b] = m.get(a, b)
				}
			}
			score += 40 * countRuns(line, append(append([]bool{}, pattern...), quiet...))
			score += 40 * countRuns(line, append(append([]bool{}, quiet...), pattern...))
		}
	}

	// Rule 4: how far the proportion of dark modules is from half.
	dark := 0
	for _, v := range m.modules {
		if v {
			dark++
		}
	}
	percent := dark * 100 / len(m.modules)
	deviation := percent - 50
	if deviation < 0 {
		deviation = -deviation
	}
	score += (deviation / 5) * 10

	return score
}

func countRuns(line, want []bool) int {
	n := 0
	for i := 0; i+len(want) <= len(line); i++ {
		match := true
		for j, v := range want {
			if line[i+j] != v {
				match = false
				break
			}
		}
		if match {
			n++
		}
	}
	return n
}
