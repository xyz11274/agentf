"""Isolated BFCL run paths; legacy data and memory stay untouched."""
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SPLIT_ID = os.environ.get("BFCL_SPLIT_ID", "")
if SPLIT_ID not in ("", "seen-scenarios-v1"):
    raise RuntimeError("bfcl_split_id_invalid")
DATA_ROOT = HERE / "datasets" / SPLIT_ID if SPLIT_ID else HERE / "datasets"
OUTPUT = ROOT / "trace_output/bfcl-v3" / SPLIT_ID if SPLIT_ID else ROOT / "trace_output/bfcl-v3"
SOURCE = DATA_ROOT / "source"
TRAINING_DATASET = DATA_ROOT / "training-reviewed"
GEPA_VIEW = DATA_ROOT / "gepa-view.json"
GEPA_SOURCE = DATA_ROOT / "gepa-source"
COLDSTART_DEPLOYMENT = HERE / (SPLIT_ID + "-coldstart-deployment" if SPLIT_ID else "coldstart-deployment")
GEPA_DEPLOYMENT = HERE / (SPLIT_ID + "-gepa-deployment" if SPLIT_ID else "gepa-deployment")
LOCK_TAG = "bfcl-v3-" + SPLIT_ID if SPLIT_ID else "bfcl-v3"
