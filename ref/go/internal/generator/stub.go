package generator

import "github.com/Andrea-Cavallo/cadenza/internal/theory"

type StyleFamily string

const (
	StyleFamilyGroove  StyleFamily = "groove"
	StyleFamilyRolling StyleFamily = "rolling"
	StyleFamilySub     StyleFamily = "sub"
)

type MusicContext struct {
	BPM              float64
	Key              theory.Key
	ChordProgression theory.ChordProgression
	VariationSeed    string
	OfflineStyle     string
	StyleFamily      StyleFamily
}

func modeFlag(k theory.Key) string {
	if k.Mode == "minor" {
		return "m"
	}
	return ""
}
