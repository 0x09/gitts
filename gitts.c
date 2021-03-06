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

#include <git2.h>
#include <sqlite3.h>

#if HAVE_BIRTHTIME
#define BTIME_OR_ZERO(st) ((st).st_birthtime)
#else
#define BTIME_OR_ZERO(st) (0)
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

	sqlite3_bind_blob(*ctx->stmt, 1, git_tree_entry_id(entry), GIT_OID_RAWSZ, SQLITE_TRANSIENT);

	if(ctx->action == TS_STORE) {
		struct stat st;
		stat(path,&st);
		sqlite3_bind_int(*ctx->stmt, 2, BTIME_OR_ZERO(st));
		sqlite3_bind_int(*ctx->stmt, 3, st.st_mtime);
		sqlite3_step(*ctx->stmt);
	}
	else if(ctx->action == TS_APPLY && sqlite3_step(*ctx->stmt) == SQLITE_ROW) {
		time_t birthtime = sqlite3_column_int(*ctx->stmt, 0);
		time_t mtime = sqlite3_column_int(*ctx->stmt, 1);
		utimes(path, (struct timeval[2]){{0},{birthtime}});
		utimes(path, (struct timeval[2]){{time(NULL)},{mtime}});
	}
#if HAVE_BIRTHTIME
	else if(ctx->action == TS_MERGE && sqlite3_step(*ctx->stmt) == SQLITE_DONE) {
		time_t leastbirthtime = LONG_MAX;
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
				sqlite3_bind_blob(*ctx->stmt, 1, git_tree_entry_id(pentry), GIT_OID_RAWSZ, SQLITE_TRANSIENT);
				if(sqlite3_step(*ctx->stmt) == SQLITE_ROW) {
					time_t birthtime = sqlite3_column_int(*ctx->stmt, 0);
					if(birthtime < leastbirthtime)
						leastbirthtime = birthtime;
				}
				git_tree_entry_free(pentry);
			}
			git_tree_free(tree);
			git_commit_free(parent);
		}
		if(leastbirthtime < LONG_MAX) {
			struct stat st;
			stat(path,&st);
			sqlite3_bind_blob(ctx->stmt[1], 1, git_tree_entry_id(entry), GIT_OID_RAWSZ, SQLITE_TRANSIENT);
			sqlite3_bind_int(ctx->stmt[1], 2, leastbirthtime);
			sqlite3_bind_int(ctx->stmt[1], 3, st.st_mtime);
			sqlite3_step(ctx->stmt[1]);
			sqlite3_reset(ctx->stmt[1]);
			utimes(path, (struct timeval[2]){{0},{leastbirthtime}});
			utimes(path, (struct timeval[2]){{time(NULL)},{st.st_mtime}});
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
		const git_error* err = giterr_last();
		fprintf(stderr,"%s (%d)\n",(err ? err->message : "Unknown error"),ret);
		return ret;
	}

	char* dbloc;
	asprintf(&dbloc,"%sts.db",git_repository_path(repo));

	if(ctx.action == TS_INIT || sqlite3_open_v2(dbloc, &ctx.db, SQLITE_OPEN_READWRITE, NULL)) {
		sqlite3_open_v2(dbloc, &ctx.db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL);
		sqlite3_exec(ctx.db,"CREATE TABLE timestamps (id BLOB PRIMARY KEY, birthtime INTEGER, mtime INTEGER);",NULL,NULL,NULL);
	}

	if(ctx.action == TS_INIT) {
		const static char* hooks[3][2] = {{"commit","store"},{"checkout","apply"},{"merge","merge"}};
		for(int i = 0; i < 3; i++) {
			char* hook;
			asprintf(&hook,"%shooks/post-%s",git_repository_path(repo),hooks[i][0]);
			FILE* f = fopen(hook,"w");
			fprintf(f,"#!/bin/sh\ngitts %s\n",hooks[i][1]);
			fclose(f);
			struct stat st;
			stat(hook,&st);
			chmod(hook,st.st_mode|S_IXUSR|S_IXGRP|S_IXOTH);
			free(hook);
		}
	}
	else {
		int stmts = 0;
		if(ctx.action == TS_APPLY || ctx.action == TS_MERGE)
			sqlite3_prepare_v2(ctx.db, "SELECT birthtime,mtime FROM timestamps WHERE id = ?", -1, ctx.stmt+stmts++, NULL);
		if(ctx.action == TS_STORE || ctx.action == TS_MERGE)
			sqlite3_prepare_v2(ctx.db, "INSERT OR IGNORE INTO timestamps VALUES(?,?,?)", -1, ctx.stmt+stmts++, NULL);

		git_reference* head;
		git_repository_head(&head,repo);
		git_reference* ref;
		git_reference_resolve(&ref,head);
		const git_oid* id = git_reference_target(ref);
		git_object* obj;
		git_object_lookup(&obj, repo, id, GIT_OBJ_COMMIT);
		git_tree* tree;
		ctx.commit = (git_commit*)obj;
		git_commit_tree(&tree, ctx.commit);

		git_tree_walk(tree, GIT_TREEWALK_PRE, treewalk, &ctx);

		git_tree_free(tree);
		git_object_free(obj);
		git_reference_free(ref);
		git_reference_free(head);
		git_repository_free(repo);
		for(int i = 0; i < stmts; i++)
			sqlite3_finalize(ctx.stmt[i]);
	}
	sqlite3_close(ctx.db);
	free(dbloc);
	return 0;
}