# MARK:Sync with core/src/middleware/logger_module.py
"""Logger - local logger with [LOCAL] prefix."""
import logging, sys

try:
    from uvicorn.logging import DefaultFormatter
    _formatter = DefaultFormatter("[LOCAL] %(levelprefix)s %(message)s", use_colors=None)
except ImportError:
    _formatter = logging.Formatter("[LOCAL] %(levelname)s: %(message)s")

log = logging.getLogger("local.logger")
if not log.handlers:
    log.setLevel(logging.INFO)
    _h = logging.StreamHandler(sys.stderr)
    _h.setFormatter(_formatter)
    log.addHandler(_h)

if __name__ == "__main__":
    log.info("Logger initialized successfully.")
    log.debug("This is a debug message (should appear).")
    log.warning("This is a warning message.")
    log.error("This is an error message.")
    log.critical("This is a critical message.")
