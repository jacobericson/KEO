"""Find the checkout folder that sits beside the resources folder (Python 3, standard library only).

The build tools' default dependencies live in a folder named resources beside the checkout, and
their defaults are written relative to such a checkout ('..\\resources\\KenshiLib'). A checkout
nested below another one, such as a worktree, is not beside it. beside_resources(start) looks
in each of the LEVELS folders above start for a resources folder holding MARKER and returns
the folder just below the nearest one that has it: start itself when resources sits beside
start, else the enclosing ancestor. A default written relative to a checkout beside resources
then resolves the same from any depth. tools\\kenshilib\\resources_root.bat makes the same search
for the batch scripts, starting at the checkout root.
"""
import os

LEVELS = 6
MARKER = os.path.join('KenshiLib', 'Libraries', 'KenshiLib', 'KenshiLib.lib')


def beside_resources(start):
    """The folder at or above start whose parent holds resources/MARKER, looking in at most
    LEVELS parents of start, nearest first; None when none of them does."""
    here = os.path.abspath(start)
    for _ in range(LEVELS):
        parent = os.path.dirname(here)
        if parent == here:
            return None
        if os.path.isfile(os.path.join(parent, 'resources', MARKER)):
            return here
        here = parent
    return None


def default_kenshilib(repo):
    """KENSHILIB's default for the checkout at repo: resources\\KenshiLib beside the folder
    beside_resources(repo) returns, else beside repo itself."""
    base = beside_resources(repo) or os.path.abspath(repo)
    return os.path.normpath(os.path.join(base, '..', 'resources', 'KenshiLib'))
