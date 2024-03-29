Simple set of git hooks for maintaining local, noninvasive, per-object file modification timestamps across checkouts.

On systems that support st_birthtime (OS X and FreeBSD), maintains birthtime across merges as well.

Timestamp metadata is stored locally, and won't clutter up commits or be pushed to remotes.

# Building
Requires libgit2 and sqlite.

    make && make install
	
# Usage
Running `gitts init` in a git working directory will install hooks to transparently record and restore file timestamps on commit, checkout, and merge.

Note that some operations which modify files (such as stashing) can't be monitored using git hooks. For these `gitts apply` can still be used directly to restore previously committed timestamps.
