#!/usr/bin/env python3
# Random key/scale/bpm/seed/style cases through both dumps; prints the first mismatches. usage: parity.py <go> <cpp> <n>
import random, subprocess, sys
GO = sys.argv[1]; CPP = sys.argv[2]; N = int(sys.argv[3])
roots = ["C","C#","Db","D","D#","Eb","E","F","F#","Gb","G","G#","Ab","A","A#","Bb","B"]
scales = ["major","minor_natural","minor_harmonic","dorian","phrygian","mixolydian","lydian"]
bpms = ["100","115","116","117","118","119","120","122","123","124","125","126","127","128","129","130","135","140","145","150"]
fams = ["groove","rolling","sub"]
styles = ["","melodic","hypnotic","driving","minimal"]
random.seed(7)
bad = 0
for n in range(N):
    a = [random.choice(roots), random.choice(scales), random.choice(bpms), str(random.randint(0,99999)) if n%3 else "%08x-%04x"%(random.getrandbits(32),random.getrandbits(16)), random.choice(fams), random.choice(styles)]
    try:
        g = subprocess.run([GO]+a, capture_output=True, text=True, timeout=5).stdout
    except subprocess.TimeoutExpired:
        print("GO HANG", a); g = "HANG"
    try:
        c = subprocess.run([CPP]+a, capture_output=True, text=True, timeout=5).stdout
    except subprocess.TimeoutExpired:
        print("CPP HANG", a); c = "HANG"
    if g != c:
        bad += 1
        if bad <= 5:
            gl, cl = g.splitlines(), c.splitlines()
            for i,(x,y) in enumerate(zip(gl,cl)):
                if x != y:
                    print("MISMATCH", a, "line", i, repr(x), repr(y)); break
            else: print("MISMATCH len", a, len(gl), len(cl))
print("cases", N, "mismatches", bad)
sys.exit(1 if bad else 0)
