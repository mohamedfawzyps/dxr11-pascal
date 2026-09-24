#!/usr/bin/env python3
"""Lower WITHOUT the NVAPI fold: the 0.41.0 behaviour. TEST ONLY.

The control for the NVAPI driver check in tools/run_rewriter_test.ps1. With
the extension slot registered, the library this writes must kill the driver,
or that check cannot tell a fixed rewriter from a broken one.

    python phase5/rewriter/nofold.py <in.ll> <out.ll>
"""

import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'hand'))

from dxil import Module
from llnorm import normalize
import lower
import rayquery

m = Module(normalize(io.open(sys.argv[1], encoding='utf-8').read()))
io.open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write(
    lower.lower(m, rayquery.analyze(m)))
