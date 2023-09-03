/*
 * gitts - preserve local timestamps for files in a git repo
 * Copyright 2013-2016 0x09.net.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>

#include <git2.h>
#include <sqlite3.h>

#ifdef __APPLE__
#define stat_nanosec(st,time) ((st).st_ ## time ## timespec.tv_nsec)
#else
#define stat_nanosec(st,time) ((st).st_ ## time ## tim.tv_nsec)
#endif

enum tsaction {
	TS_STORE,
	TS_APPLY,
	TS_MERGE,
	TS_INIT
};

struct tscontext {
	enum tsaction action;
	const char* path;
	sqlite3* db;
	sqlite3_stmt* stmt[2];
	git_commit* commit;
};

int treewalk(const char* root, const git_tree_entry* entry, void* payload) {
	struct tscontext* ctx = payload;
	char* path;
	asprintf(&path,"%s/%s%s",ctx->path,root,git_tree_entry_name(entry));

	sqlite3_bind_blob(*ctx->stmt, 1, git_tree_entry_id(entry)->id, GIT_OID_RAWSZ, SQLITE_TRANSIENT);

	if(ctx->action == TS_STORE) {
		struct stat st;
		stat(path,&st);
#if HAVE_BIRTHTIME
		sqlite3_bind_int64(*ctx->stmt, 2, st.st_birthtime);
		sqlite3_bind_int64(*ctx->stmt, 4, stat_nanosec(st,birth));
#endif
		sqlite3_bind_int64(*ctx->stmt, 3, st.st_mtime);
		sqlite3_bind_int64(*ctx->stmt, 5, stat_nanosec(st,m));
		sqlite3_step(*ctx->stmt);
	}
	else if(ctx->action == TS_APPLY && sqlite3_step(*ctx->stmt) == SQLITE_ROW) {
#if HAVE_BIRTHTIME
		if(sqlite3_column_type(*ctx->stmt, 0) != SQLITE_NULL) {
			struct timespec birthtime = { sqlite3_column_int64(*ctx->stmt, 0), sqlite3_column_int64(*ctx->stmt, 2) };
			utimensat(AT_FDCWD, path, (struct timespec[2]){{0,UTIME_OMIT},birthtime}, AT_SYMLINK_NOFOLLOW);
		}
#endif
		struct timespec mtime = { sqlite3_column_int64(*ctx->stmt, 1), sqlite3_column_int64(*ctx->stmt, 3) };
		utimensat(AT_FDCWD, path, (struct timespec[2]){{0,UTIME_NOW},mtime}, AT_SYMLINK_NOFOLLOW);
	}
#if HAVE_BIRTHTIME
	else if(ctx->action == TS_MERGE && sqlite3_step(*ctx->stmt) == SQLITE_DONE) {
		struct timespec leastbirthtime = {LONG_MAX,LONG_MAX};
		unsigned int parents = git_commit_parentcount(ctx->commit);
		git_commit* parent;
		const char* localpath = path + strlen(ctx->path) + 1;
		for(unsigned int i = 0; i < parents; i++) {
			git_commit_parent(&parent, ctx->commit, i);
			git_tree* tree;
			git_commit_tree(&tree, parent);
			git_tree_entry* pentry;
			if(git_tree_entry_bypath(&pentry, tree, localpath) != GIT_ENOTFOUND) {
				sqlite3_reset(*ctx->stmt);
				sqlite3_bind_blob(*ctx->stmt, 1, git_tree_entry_id(pentry)->id, GIT_OID_RAWSZ, SQLITE_TRANSIENT);
				if(sqlite3_step(*ctx->stmt) == SQLITE_ROW) {
					struct timespec birthtime = { sqlite3_column_int64(*ctx->stmt, 0), sqlite3_column_int64(*ctx->stmt, 2) };
					if(birthtime.tv_sec < leastbirthtime.tv_sec || birthtime.tv_sec == leastbirthtime.tv_sec && birthtime.tv_nsec < leastbirthtime.tv_nsec)
						leastbirthtime = birthtime;
				}
				git_tree_entry_free(pentry);
			}
			git_tree_free(tree);
			git_commit_free(parent);
		}
		if(leastbirthtime.tv_nsec < LONG_MAX) {
			struct stat st;
			stat(path,&st);
			sqlite3_bind_blob(ctx->stmt[1], 1, git_tree_entry_id(entry)->id, GIT_OID_RAWSZ, SQLITE_TRANSIENT);
			sqlite3_bind_int64(ctx->stmt[1], 2, leastbirthtime.tv_sec);
			sqlite3_bind_int64(ctx->stmt[1], 3, st.st_mtime);
			sqlite3_bind_int64(ctx->stmt[1], 4, leastbirthtime.tv_nsec);
			sqlite3_bind_int64(ctx->stmt[1], 5, stat_nanosec(st,m));
			sqlite3_step(ctx->stmt[1]);
			sqlite3_reset(ctx->stmt[1]);
			utimensat(AT_FDCWD, path, (struct timespec[2]){{0,UTIME_OMIT},leastbirthtime}, AT_SYMLINK_NOFOLLOW);
			utimensat(AT_FDCWD, path, (struct timespec[2]){{0,UTIME_NOW},{st.st_mtime, stat_nanosec(st,m)}}, AT_SYMLINK_NOFOLLOW);
		}
	}
#endif
	sqlite3_reset(*ctx->stmt);
	free(path);
	return 0;
}

void usage() {
	fprintf(stderr,"Usage: gitts [init|store|apply|merge] (.)\n");
	exit(1);
}
int main(int argc, char* argv[]) {
	if(argc < 2)
		usage();
	struct tscontext ctx = { .path = argc < 3 ? "." : argv[2] };
	if(!strcmp(argv[1],"apply"))
		ctx.action = TS_APPLY;
	else if(!strcmp(argv[1],"store"))
		ctx.action = TS_STORE;
	else if(!strcmp(argv[1],"merge"))
		ctx.action = TS_MERGE;
	else if(!strcmp(argv[1],"init"))
		ctx.action = TS_INIT;
	else usage();

	git_libgit2_init();
	git_repository* repo;
	int ret = git_repository_open(&repo, ctx.path);
	if(ret < 0) {
		const git_error* err = git_error_last();
		fprintf(stderr,"%s (%d)\n",(err ? err->message : "Unknown error"),ret);
		return 1;
	}

	char* dbloc;
	asprintf(&dbloc,"%sts.db",git_repository_path(repo));

	if(ctx.action == TS_INIT || sqlite3_open_v2(dbloc, &ctx.db, SQLITE_OPEN_READWRITE, NULL)) {
		sqlite3_open_v2(dbloc, &ctx.db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL);
		sqlite3_exec(ctx.db,"CREATE TABLE timestamps (id BLOB PRIMARY KEY, birthtime INTEGER, mtime INTEGER, birthtime_nsec INTEGER, mtime_nsec INTEGER);",NULL,NULL,NULL);
	}
#ifndef GITTS_SKIP_NANOSECOND_SCHEMA_CHECK
	else if(sqlite3_table_column_metadata(ctx.db,NULL,"timestamps","mtime_nsec",NULL,NULL,NULL,NULL,NULL)) // add nanosecond timestamps to old dbs
		sqlite3_exec(ctx.db,"ALTER TABLE timestamps ADD birthtime_nsec INTEGER; ALTER TABLE timestamps ADD mtime_nsec INTEGER;",NULL,NULL,NULL);
#endif

	if(ctx.action == TS_INIT) {
		char* hookspath;
		git_config* cfg = NULL;
		git_buf out = {0};
		if(!git_repository_config(&cfg, repo) && !git_config_get_path(&out, cfg, "core.hooksPath"))
			asprintf(&hookspath, "%s", out.ptr);
		else
			asprintf(&hookspath, "%shooks", git_repository_path(repo));
		git_buf_dispose(&out);
		git_config_free(cfg);

		const static char* hooks[3][2] = {{"commit","store"},{"checkout","apply"},{"merge","merge"}};
		for(int i = 0; i < 3; i++) {
			char* hook;
			asprintf(&hook, "%s/post-%s", hookspath, hooks[i][0]);
			int fd = open(hook, O_CREAT|O_WRONLY|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH|S_IXUSR|S_IXGRP|S_IXOTH);
			if(fd < 0) {
				if(errno == EEXIST)
					fprintf(stderr, "gitts: hook already exists, not overwriting. If you haven't already, add \"gitts %s\" to %s\n", hooks[i][1], hook);
				else
					fprintf(stderr, "gitts: %s: %s\n", hook, strerror(errno));
				ret = 1;
			}
			else {
				FILE* f = fdopen(fd, "w");
				fprintf(f,"#!/bin/sh\ngitts %s\n",hooks[i][1]);
				fclose(f);
			}
			free(hook);
		}
		free(hookspath);
	}
	else {
		int stmts = 0;
		if(ctx.action == TS_APPLY || ctx.action == TS_MERGE)
			sqlite3_prepare_v2(ctx.db, "SELECT birthtime,mtime,birthtime_nsec,mtime_nsec FROM timestamps WHERE id = ?", -1, ctx.stmt+stmts++, NULL);
		if(ctx.action == TS_STORE || ctx.action == TS_MERGE)
			sqlite3_prepare_v2(ctx.db, "INSERT OR IGNORE INTO timestamps VALUES(?,?,?,?,?)", -1, ctx.stmt+stmts++, NULL);

		git_reference* head;
		git_tree* tree = NULL;
		if((ret = git_repository_head(&head, repo)) ||
		   (ret = git_commit_lookup(&ctx.commit, repo, git_reference_target(head))) ||
		   (ret = git_commit_tree(&tree, ctx.commit))) {
			const git_error* err = git_error_last();
			fprintf(stderr, "%s (%d)\n", (err ? err->message : "Unknown error"), ret);
		}
		else
			git_tree_walk(tree, GIT_TREEWALK_PRE, treewalk, &ctx);

		git_tree_free(tree);
		git_commit_free(ctx.commit);
		git_reference_free(head);
		git_repository_free(repo);
		for(int i = 0; i < stmts; i++)
			sqlite3_finalize(ctx.stmt[i]);
	}
	sqlite3_close(ctx.db);
	free(dbloc);
	return !!ret;
}
