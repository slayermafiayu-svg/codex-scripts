import os

# Tests use temporary roots (C: on Windows CI); the D: rule is tested explicitly in test_core.py.
os.environ["AIMOTION_ALLOW_ANY_DRIVE"] = "1"
