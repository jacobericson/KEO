"""Throwaway git repositories for tests, each copied from a template made once per process.

`git init` and every `git config` start a git process. A repository made here costs none after
the first of its configuration: the template is `git init` from an empty template directory (no
sample hooks) plus the same config rows in the same order, and each new repository is a copy of
its `.git`. The templates are removed when the process exits. read_head answers
`git rev-parse HEAD` from the repository's files where it can, also without a process. rmtree
removes a folder of test repositories, read-only object files included.
"""
import atexit
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

# Variables that would point git at a repository other than the one being made.
_REPO_VARS = ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY')
_HEX = frozenset('0123456789abcdef')

_templates = {}
_scratch = []


# Waits before each retry of a refused removal: a scanner or indexer can hold a file git just wrote
# for a moment. The attempt after the last wait raises.
_RETRY_DELAYS = (0, 0.05, 0.1, 0.2, 0.4)


def _writable_retry(func, path, _exc):
    for delay in _RETRY_DELAYS:
        time.sleep(delay)
        try:
            os.chmod(path, stat.S_IWRITE)
            func(path)
            return
        except FileNotFoundError:
            return
        except PermissionError:
            pass
    os.chmod(path, stat.S_IWRITE)
    func(path)


def rmtree(path):
    """Removes a tree holding git repositories. Git makes its object files read-only, which
    Windows refuses to delete, so each refused entry is made writable and removed again; any other
    failure raises. A path that is already gone is not an error."""
    if not os.path.lexists(path):
        return
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=_writable_retry)
    else:
        shutil.rmtree(path, onerror=_writable_retry)


def _make_template(branch, config):
    if not _scratch:
        _scratch.append(tempfile.mkdtemp(prefix='git-fixture-'))
        atexit.register(rmtree, _scratch[0])
    base = tempfile.mkdtemp(dir=_scratch[0])
    empty = os.path.join(base, 'empty')
    repo = os.path.join(base, 'repo')
    os.mkdir(empty)
    env = dict(os.environ)
    for name in _REPO_VARS:
        env.pop(name, None)
    argv = ['git', 'init', '-q', '--template=' + empty] + (['-b', branch] if branch else []) + [repo]
    subprocess.check_call(argv, env=env, stdout=subprocess.DEVNULL)
    for key, value in config:
        subprocess.check_call(['git', '-C', repo, 'config', key, value], env=env)
    git_dir = os.path.join(repo, '.git')
    # A default init has both directories; tests install hooks in the first.
    for name in ('hooks', 'info'):
        os.makedirs(os.path.join(git_dir, name), exist_ok=True)
    return git_dir


def init(root, config=(), branch=None):
    """Makes the existing directory `root` a new repository, as `git init -q` (with `-b branch`
    when given) followed by `git config key value` for each (key, value) of `config` in order."""
    key = (branch, tuple(tuple(row) for row in config))
    if key not in _templates:
        _templates[key] = _make_template(branch, key[1])
    shutil.copytree(_templates[key], os.path.join(root, '.git'))


def read_head(root):
    """The commit `root`'s HEAD names when HEAD is a branch whose ref is a loose file, read without
    starting git; None otherwise (packed or unborn ref, detached HEAD, a `.git` file), so the caller
    asks git instead."""
    git_dir = os.path.join(root, '.git')
    try:
        with open(os.path.join(git_dir, 'HEAD'), 'rb') as f:
            head = f.read().decode('ascii').strip()
        if not head.startswith('ref: refs/heads/'):
            return None
        with open(os.path.join(git_dir, *head[len('ref: '):].split('/')), 'rb') as f:
            sha = f.read().decode('ascii').strip()
    except (OSError, UnicodeDecodeError):
        return None
    return sha if len(sha) == 40 and set(sha) <= _HEX else None
