"""Throwaway git repositories for tests, each copied from a template made once per process.

`git init` and every `git config` start a git process. A repository made here costs none after
the first of its configuration: the template is `git init` from an empty template directory (no
sample hooks) plus the same config rows in the same order, and each new repository is a copy of
its `.git`. The templates are removed when the process exits. read_head answers
`git rev-parse HEAD` from the repository's files where it can, also without a process.
"""
import atexit
import os
import shutil
import subprocess
import tempfile

# Variables that would point git at a repository other than the one being made.
_REPO_VARS = ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY')
_HEX = frozenset('0123456789abcdef')

_templates = {}
_scratch = []


def _make_template(branch, config):
    if not _scratch:
        _scratch.append(tempfile.mkdtemp(prefix='git-fixture-'))
        atexit.register(shutil.rmtree, _scratch[0], True)
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
