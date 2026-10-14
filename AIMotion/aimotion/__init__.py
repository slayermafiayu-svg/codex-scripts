"""AI Motion for VEGAS Pro.

Generates character motion with locally run pre-trained models and hands it
to VEGAS either as Puppet Pin keyframes (editable) or as rendered frames.

Import order matters: ``aimotion.paths.apply_environment()`` must run before
torch / transformers / diffusers are imported so that every model, cache and
temporary file lands under the configured root (D:\\AIMotion by default).
"""

__version__ = "0.1.0"
