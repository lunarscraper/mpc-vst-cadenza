// Reference dump of Cadenza's offline engine: same stems, same profile lookup as renderAndSaveNamed.
// usage: dump <root> <scale> <bpm> <seed> <family groove|rolling|sub> [offlineStyle]
package main

import (
	"fmt"
	"os"
	"strconv"

	"github.com/Andrea-Cavallo/cadenza/internal/generator"
	"github.com/Andrea-Cavallo/cadenza/internal/midi"
	"github.com/Andrea-Cavallo/cadenza/internal/renderer"
	"github.com/Andrea-Cavallo/cadenza/internal/renderer/styleprofile"
	"github.com/Andrea-Cavallo/cadenza/internal/theory"
)

func main() {
	root, scale := os.Args[1], os.Args[2]
	bpm, _ := strconv.ParseFloat(os.Args[3], 64)
	seed, fam := os.Args[4], os.Args[5]
	ost := ""
	if len(os.Args) > 6 {
		ost = os.Args[6]
	}
	mode := scale
	switch scale {
	case "minor_natural":
		mode = "minor"
	case "major":
		mode = "major"
	}
	key := theory.Key{Root: root, Mode: mode, Scale: scale}
	prog := theory.SelectProgression(root, scale, seed)
	fmt.Printf("PROG")
	for _, c := range prog.Chords {
		fmt.Printf(" %s%s", c.Root, theory.QualitySuffix(c.Quality))
	}
	fmt.Println()
	ctx := generator.MusicContext{BPM: bpm, Key: key, ChordProgression: prog, VariationSeed: seed, OfflineStyle: ost, StyleFamily: generator.StyleFamily(fam)}
	reg := styleprofile.NewRegistry()
	r := renderer.New()
	for _, pt := range []string{"bassline", "bassline_rolling", "bassline_sub", "arpeggio", "melody", "chord_pad", "lead_stab"} {
		spec := generator.OfflineTemplate(pt, ctx)
		prof, err := reg.LoadForType(pt, spec.StyleProfile)
		if err != nil {
			prof, _ = reg.DefaultForTypeExtended(pt)
		}
		ev, _ := r.Render(spec, prof)
		fmt.Printf("STEM %s %s\n", pt, prof.Name)
		for _, e := range ev {
			switch e.Type {
			case midi.NoteOn:
				fmt.Printf("N %d %d %d\n", e.Tick, e.Note, e.Velocity)
			case midi.NoteOff:
				fmt.Printf("O %d %d\n", e.Tick, e.Note)
			case midi.ControlChange:
				fmt.Printf("C %d %d %d\n", e.Tick, e.Controller, e.Value)
			}
		}
	}
}
