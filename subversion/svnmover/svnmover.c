/*
 * svnmover.c: Concept Demo for Move Tracking and Branching
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <apr_lib.h>

#include "svn_private_config.h"
#include "svn_hash.h"
#include "svn_iter.h"
#include "svn_client.h"
#include "svn_cmdline.h"
#include "svn_config.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_string.h"
#include "svn_subst.h"
#include "svn_utf.h"
#include "svn_version.h"
#include "svnmover.h"

#include "private/svn_cmdline_private.h"
#include "private/svn_subr_private.h"
#include "private/svn_branch_repos.h"
#include "private/svn_branch_nested.h"
#include "private/svn_branch_compat.h"
#include "private/svn_ra_private.h"
#include "private/svn_string_private.h"
#include "private/svn_sorts_private.h"
#include "private/svn_token.h"
#include "private/svn_client_private.h"
#include "../libsvn_delta/debug_editor.h"

#define HAVE_LINENOISE
#ifdef HAVE_LINENOISE
#include "../libsvn_subr/linenoise/linenoise.h"
#endif

/* Version compatibility check */
static svn_error_t *
check_lib_versions(void)
{
  static const svn_version_checklist_t checklist[] =
    {
      { "svn_client", svn_client_version },
      { "svn_subr",   svn_subr_version },
      { "svn_ra",     svn_ra_version },
      { NULL, NULL }
    };
  SVN_VERSION_DEFINE(my_version);

  return svn_ver_check_list2(&my_version, checklist, svn_ver_equal);
}

static svn_boolean_t quiet = FALSE;

/* UI mode: whether to display output in terms of paths or elements */
enum { UI_MODE_EIDS, UI_MODE_PATHS, UI_MODE_SERIAL };
static int the_ui_mode = UI_MODE_EIDS;
static const svn_token_map_t ui_mode_map[]
  = { {"eids", UI_MODE_EIDS},
      {"e", UI_MODE_EIDS},
      {"paths", UI_MODE_PATHS},
      {"p", UI_MODE_PATHS},
      {"serial", UI_MODE_SERIAL},
      {"s", UI_MODE_SERIAL},
      {NULL, SVN_TOKEN_UNKNOWN} };

#define is_branch_root_element(branch, eid) \
  (svn_branch_root_eid(branch) == (eid))

/* Is BRANCH1 the same branch as BRANCH2? Compare by full branch-ids; don't
   require identical branch objects. */
#define BRANCH_IS_SAME_BRANCH(branch1, branch2, scratch_pool) \
  (strcmp(svn_branch_get_id(branch1, scratch_pool), \
          svn_branch_get_id(branch2, scratch_pool)) == 0)

/* Print a notification. */
__attribute__((format(printf, 1, 2)))
static void
notify(const char *fmt,
       ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

/* Print a verbose notification: in 'quiet' mode, don't print it. */
__attribute__((format(printf, 1, 2)))
static void
notify_v(const char *fmt,
         ...)
{
  va_list ap;

  if (! quiet)
    {
      va_start(ap, fmt);
      vprintf(fmt, ap);
      va_end(ap);
      printf("\n");
    }
}

#define SVN_CL__LOG_SEP_STRING \
  "------------------------------------------------------------------------\n"

/* ====================================================================== */

/* Update the WC to revision BASE_REVISION (SVN_INVALID_REVNUM means HEAD).
 *
 * Requires these fields in WC:
 *   head_revision
 *   repos_root_url
 *   ra_session
 *   pool
 *
 * Initializes these fields in WC:
 *   base_revision
 *   base_branch_id
 *   base_branch
 *   working_branch_id
 *   working_branch
 *   editor
 *
 * Assumes there are no changes in the WC: throws away the existing txn
 * and starts a new one.
 */
static svn_error_t *
wc_checkout(svnmover_wc_t *wc,
            svn_revnum_t base_revision,
            const char *base_branch_id,
            apr_pool_t *scratch_pool)
{
  const char *branch_info_dir = NULL;
  svn_branch_compat__shim_fetch_func_t fetch_func;
  void *fetch_baton;
  svn_branch_txn_t *base_txn;

  /* Validate and store the new base revision number */
  if (! SVN_IS_VALID_REVNUM(base_revision))
    base_revision = wc->head_revision;
  else if (base_revision > wc->head_revision)
    return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                             _("No such revision %ld (HEAD is %ld)"),
                             base_revision, wc->head_revision);

  /* Choose whether to store branching info in a local dir or in revprops.
     (For now, just to exercise the options, we choose local files for
     RA-local and revprops for a remote repo.) */
  if (strncmp(wc->repos_root_url, "file://", 7) == 0)
    {
      const char *repos_dir;

      SVN_ERR(svn_uri_get_dirent_from_file_url(&repos_dir, wc->repos_root_url,
                                               scratch_pool));
      branch_info_dir = svn_dirent_join(repos_dir, "branch-info", scratch_pool);
    }

  /* Get a mutable transaction based on that rev. (This implementation
     re-reads all the move-tracking data from the repository.) */
  SVN_ERR(svn_ra_load_branching_state(&wc->edit_txn,
                                      &fetch_func, &fetch_baton,
                                      wc->ra_session, branch_info_dir,
                                      base_revision,
                                      wc->pool, scratch_pool));

  wc->edit_txn = svn_nested_branch_txn_create(wc->edit_txn, wc->pool);

  /* Store the WC base state */
  base_txn = svn_branch_repos_get_base_revision_root(wc->edit_txn);
  wc->base = apr_pcalloc(wc->pool, sizeof(*wc->base));
  wc->base->revision = base_revision;
  wc->base->branch_id = apr_pstrdup(wc->pool, base_branch_id);
  wc->base->branch
    = svn_branch_txn_get_branch_by_id(base_txn, wc->base->branch_id,
                                      scratch_pool);
  if (! wc->base->branch)
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                             "Cannot check out WC: branch %s not found in r%ld",
                             base_branch_id, base_revision);

  wc->working = apr_pcalloc(wc->pool, sizeof(*wc->working));
  wc->working->revision = SVN_INVALID_REVNUM;
  wc->working->branch_id = wc->base->branch_id;
  wc->working->branch
    = svn_branch_txn_get_branch_by_id(wc->edit_txn, wc->working->branch_id,
                                      scratch_pool);
  SVN_ERR_ASSERT(wc->working->branch);

  return SVN_NO_ERROR;
}

/* Create a simulated WC, in memory.
 *
 * Initializes these fields in WC:
 *   head_revision
 *   repos_root_url
 *   ra_session
 *   made_changes
 *   ctx
 *   pool
 *
 * BASE_REVISION is the revision to work on, or SVN_INVALID_REVNUM for HEAD.
 */
static svn_error_t *
wc_create(svnmover_wc_t **wc_p,
          const char *anchor_url,
          svn_revnum_t base_revision,
          const char *base_branch_id,
          svn_client_ctx_t *ctx,
          apr_pool_t *result_pool,
          apr_pool_t *scratch_pool)
{
  apr_pool_t *wc_pool = svn_pool_create(result_pool);
  svnmover_wc_t *wc = apr_pcalloc(wc_pool, sizeof(*wc));

  wc->pool = wc_pool;
  wc->ctx = ctx;

  SVN_ERR(svn_client_open_ra_session2(&wc->ra_session, anchor_url,
                                      NULL /* wri_abspath */, ctx,
                                      wc_pool, scratch_pool));

  SVN_ERR(svn_ra_get_repos_root2(wc->ra_session, &wc->repos_root_url,
                                 result_pool));
  SVN_ERR(svn_ra_get_latest_revnum(wc->ra_session, &wc->head_revision,
                                   scratch_pool));
  SVN_ERR(svn_ra_reparent(wc->ra_session, wc->repos_root_url, scratch_pool));

  SVN_ERR(wc_checkout(wc, base_revision, base_branch_id, scratch_pool));
  *wc_p = wc;
  return SVN_NO_ERROR;
}

/* Return (left, right) pairs of element content that differ between
 * subtrees LEFT and RIGHT.
 *
 * Set *DIFF_P to a hash of (eid -> (svn_branch_el_rev_content_t *)[2]).
 */
static svn_error_t *
element_differences(apr_hash_t **diff_p,
                    const svn_element_tree_t *left,
                    const svn_element_tree_t *right,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  apr_hash_t *diff = apr_hash_make(result_pool);
  SVN_ITER_T(int) *hi;

  /*SVN_DBG(("svn_branch_subtree_differences(b%s r%ld, b%s r%ld, e%d)",
           svn_branch_get_id(left->branch, scratch_pool), left->rev,
           svn_branch_get_id(right->branch, scratch_pool), right->rev,
           right->eid));*/

  for (SVN_HASH_ITER(hi, scratch_pool,
                     apr_hash_overlay(scratch_pool,
                                      left->e_map, right->e_map)))
    {
      int e = svn_int_hash_this_key(hi->apr_hi);
      svn_element_content_t *element_left
        = svn_element_tree_get(left, e);
      svn_element_content_t *element_right
        = svn_element_tree_get(right, e);

      if (! svn_element_content_equal(element_left, element_right,
                                      scratch_pool))
        {
          svn_element_content_t **contents
            = apr_palloc(result_pool, 2 * sizeof(void *));

          contents[0] = element_left;
          contents[1] = element_right;
          svn_int_hash_set(diff, e, contents);
        }
    }

  *diff_p = diff;
  return SVN_NO_ERROR;
}

/* Set *IS_CHANGED to true if EDIT_TXN differs from its base txn, else to
 * false.
 */
static svn_error_t *
txn_is_changed(svn_branch_txn_t *edit_txn,
               svn_boolean_t *is_changed,
               apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_state_t) *bi;
  svn_branch_txn_t *base_txn
    = svn_branch_repos_get_base_revision_root(edit_txn);
  apr_array_header_t *edit_branches
    = svn_branch_txn_get_branches(edit_txn, scratch_pool);
  apr_array_header_t *base_branches
    = svn_branch_txn_get_branches(base_txn, scratch_pool);

  *is_changed = FALSE;

  /* If any previous branch is now missing, that's a change. */
  for (SVN_ARRAY_ITER(bi, base_branches, scratch_pool))
    {
      svn_branch_state_t *base_branch = bi->val;
      svn_branch_state_t *edit_branch
        = svn_branch_txn_get_branch_by_id(edit_txn, base_branch->bid,
                                          scratch_pool);

      if (! edit_branch)
        {
          *is_changed = TRUE;
          return SVN_NO_ERROR;
        }
    }

  /* If any current branch is new or changed, that's a change. */
  for (SVN_ARRAY_ITER(bi, edit_branches, scratch_pool))
    {
      svn_branch_state_t *edit_branch = bi->val;
      svn_branch_state_t *base_branch
        = svn_branch_txn_get_branch_by_id(base_txn, edit_branch->bid,
                                          scratch_pool);
      apr_hash_t *diff;

      if (! base_branch)
        {
          *is_changed = TRUE;
          return SVN_NO_ERROR;
        }

      SVN_ERR(element_differences(&diff,
                                  svn_branch_get_element_tree(edit_branch),
                                  svn_branch_get_element_tree(base_branch),
                                  scratch_pool, scratch_pool));
      if (apr_hash_count(diff))
        {
          *is_changed = TRUE;
          return SVN_NO_ERROR;
        }
    }

  return SVN_NO_ERROR;
}

/* Replay differences between S_LEFT and S_RIGHT into EDITOR:EDIT_BRANCH.
 *
 * S_LEFT and/or S_RIGHT may be null meaning an empty set.
 *
 * Non-recursive: single branch only.
 */
static svn_error_t *
subtree_replay(svn_branch_state_t *edit_branch,
               const svn_element_tree_t *s_left,
               const svn_element_tree_t *s_right,
               apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_left_right;
  apr_hash_index_t *hi;

  if (! s_left)
    s_left = svn_element_tree_create(NULL, 0 /*root_eid*/, scratch_pool);
  if (! s_right)
    s_right = svn_element_tree_create(NULL, 0 /*root_eid*/, scratch_pool);

  SVN_ERR(element_differences(&diff_left_right,
                              s_left, s_right,
                              scratch_pool, scratch_pool));

  /* Go through the per-element differences. */
  for (hi = apr_hash_first(scratch_pool, diff_left_right);
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      svn_element_content_t **e_pair = apr_hash_this_val(hi);
      svn_element_content_t *e0 = e_pair[0], *e1 = e_pair[1];

      SVN_ERR_ASSERT(!e0
                     || svn_element_payload_invariants(e0->payload));
      SVN_ERR_ASSERT(!e1
                     || svn_element_payload_invariants(e1->payload));
      if (e0 || e1)
        {
          if (e0 && e1)
            {
              SVN_DBG(("replay: alter e%d", eid));
              SVN_ERR(svn_branch_state_alter_one(edit_branch, eid,
                                                 e1->parent_eid, e1->name,
                                                 e1->payload, scratch_pool));
            }
          else if (e0)
            {
              SVN_DBG(("replay: delete e%d", eid));
              SVN_ERR(svn_branch_state_delete_one(edit_branch, eid,
                                                  scratch_pool));
            }
          else
            {
              SVN_DBG(("replay: instan. e%d", eid));
              SVN_ERR(svn_branch_state_alter_one(edit_branch, eid,
                                                 e1->parent_eid, e1->name,
                                                 e1->payload, scratch_pool));
            }
        }
    }

  return SVN_NO_ERROR;
}

/*  */
static apr_hash_t *
get_union_of_subbranches(svn_branch_state_t *left_branch,
                         svn_branch_state_t *right_branch,
                         apr_pool_t *result_pool)
{
  apr_hash_t *all_subbranches;

  svn_branch_subtree_t *s_left
    = left_branch ? svn_branch_get_subtree(left_branch,
                                           svn_branch_root_eid(left_branch),
                                           result_pool) : NULL;
  svn_branch_subtree_t *s_right
    = right_branch ? svn_branch_get_subtree(right_branch,
                                            svn_branch_root_eid(right_branch),
                                            result_pool) : NULL;
  all_subbranches
    = left_branch ? apr_hash_overlay(result_pool,
                                     s_left->subbranches, s_right->subbranches)
                  : s_right->subbranches;

  return all_subbranches;
}

/* Replay differences between S_LEFT and S_RIGHT into EDITOR:EDIT_BRANCH.
 *
 * S_LEFT or S_RIGHT (but not both) may be null meaning an empty set.
 *
 * Recurse into subbranches.
 */
static svn_error_t *
svn_branch_replay(svn_branch_txn_t *edit_txn,
                  svn_branch_state_t *edit_branch,
                  svn_branch_state_t *left_branch,
                  svn_branch_state_t *right_branch,
                  apr_pool_t *scratch_pool)
{
  assert((left_branch && right_branch)
         ? (svn_branch_root_eid(left_branch) == svn_branch_root_eid(right_branch))
         : (left_branch || right_branch));

  if (right_branch)
    {
      /* Replay this branch */
      const svn_element_tree_t *s_left
        = left_branch ? svn_branch_get_element_tree(left_branch) : NULL;
      const svn_element_tree_t *s_right
        = right_branch ? svn_branch_get_element_tree(right_branch) : NULL;

      SVN_ERR(subtree_replay(edit_branch, s_left, s_right,
                             scratch_pool));
    }
  else
    {
      /* deleted branch LEFT */
      /* nothing to do -- it will go away because we deleted the outer-branch
         element where it was attached */
    }

  /* Replay its subbranches, recursively.
     (If we're deleting the current branch, we don't also need to
     explicitly delete its subbranches... do we?) */
  if (right_branch)
    {
      apr_hash_t *all_subbranches
        = get_union_of_subbranches(left_branch, right_branch, scratch_pool);
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(scratch_pool, all_subbranches);
           hi; hi = apr_hash_next(hi))
        {
          int this_eid = svn_int_hash_this_key(hi);
          svn_branch_state_t *left_subbranch
            = left_branch ? svn_branch_get_subbranch_at_eid(
                              left_branch, this_eid, scratch_pool) : NULL;
          svn_branch_state_t *right_subbranch
            = right_branch ? svn_branch_get_subbranch_at_eid(
                               right_branch, this_eid, scratch_pool) : NULL;
          svn_branch_state_t *edit_subbranch = NULL;

          /* If the subbranch is to be edited or added, first look up the
             corresponding edit subbranch, or, if not found, create one. */
          if (right_subbranch)
            {
              const char *new_branch_id
                = svn_branch_id_nest(edit_branch->bid, this_eid, scratch_pool);

              SVN_ERR(svn_branch_txn_open_branch(edit_txn, &edit_subbranch,
                                                 right_subbranch->predecessor,
                                                 new_branch_id,
                                                 svn_branch_root_eid(right_subbranch),
                                                 scratch_pool, scratch_pool));
            }

          /* recurse */
          if (edit_subbranch)
            {
              SVN_ERR(svn_branch_replay(edit_txn, edit_subbranch,
                                        left_subbranch, right_subbranch,
                                        scratch_pool));
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Replay differences between LEFT_BRANCH and RIGHT_BRANCH into
 * EDIT_ROOT_BRANCH.
 * (Recurse into subbranches.)
 */
static svn_error_t *
replay(svn_branch_txn_t *edit_txn,
       svn_branch_state_t *edit_root_branch,
       svn_branch_state_t *left_branch,
       svn_branch_state_t *right_branch,
       apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(left_branch || right_branch);

  SVN_ERR(svn_branch_replay(edit_txn, edit_root_branch,
                            left_branch, right_branch, scratch_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool);

/* Baton for commit_callback(). */
typedef struct commit_callback_baton_t
{
  svn_branch_txn_t *edit_txn;
  const char *wc_base_branch_id;
  const char *wc_commit_branch_id;

  /* just-committed revision */
  svn_revnum_t revision;
} commit_callback_baton_t;

static svn_error_t *
display_diff_of_commit(const commit_callback_baton_t *ccbb,
                       apr_pool_t *scratch_pool);

static svn_error_t *
do_topbranch(svn_branch_state_t **new_branch_p,
             svn_branch_txn_t *txn,
             svn_branch_rev_bid_eid_t *from,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool);

/* Allocate the same number of new EIDs in NEW_TXN as are already
 * allocated in OLD_TXN.
 */
static svn_error_t *
allocate_eids(svn_branch_txn_t *new_txn,
              const svn_branch_txn_t *old_txn,
              apr_pool_t *scratch_pool)
{
  int num_new_eids;
  int i;

  SVN_ERR(svn_branch_txn_get_num_new_eids(old_txn, &num_new_eids,
                                          scratch_pool));
  for (i = 0; i < num_new_eids; i++)
    {
      SVN_ERR(svn_branch_txn_new_eid(new_txn, NULL, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Commit the changes from WC into the repository.
 *
 * Open a new commit txn to the repo. Replay the changes from WC into it.
 *
 * Set WC->head_revision and *NEW_REV_P to the committed revision number.
 *
 * If there are no changes to commit, set *NEW_REV_P to SVN_INVALID_REVNUM
 * and do not make a commit and do not change WC->head_revision.
 *
 * NEW_REV_P may be null if not wanted.
 */
static svn_error_t *
wc_commit(svn_revnum_t *new_rev_p,
          svnmover_wc_t *wc,
          apr_hash_t *revprops,
          apr_pool_t *scratch_pool)
{
  const char *branch_info_dir = NULL;
  svn_branch_txn_t *commit_txn;
  commit_callback_baton_t ccbb;
  svn_boolean_t change_detected;
  const char *edit_root_branch_id;
  svn_branch_state_t *edit_root_branch;

  /* If no log msg provided, use the list of commands */
  if (! svn_hash_gets(revprops, SVN_PROP_REVISION_LOG) && wc->list_of_commands)
    {
      /* Avoid modifying the passed-in revprops hash */
      revprops = apr_hash_copy(scratch_pool, revprops);

      svn_hash_sets(revprops, SVN_PROP_REVISION_LOG,
                    svn_string_create(wc->list_of_commands, scratch_pool));
    }

  /* Choose whether to store branching info in a local dir or in revprops.
     (For now, just to exercise the options, we choose local files for
     RA-local and revprops for a remote repo.) */
  if (strncmp(wc->repos_root_url, "file://", 7) == 0)
    {
      const char *repos_dir;

      SVN_ERR(svn_uri_get_dirent_from_file_url(&repos_dir, wc->repos_root_url,
                                               scratch_pool));
      branch_info_dir = svn_dirent_join(repos_dir, "branch-info", scratch_pool);
    }

  /* Start a new editor for the commit. */
  SVN_ERR(svn_ra_get_commit_txn(wc->ra_session,
                                &commit_txn,
                                revprops,
                                commit_callback, &ccbb,
                                NULL /*lock_tokens*/, FALSE /*keep_locks*/,
                                branch_info_dir,
                                scratch_pool));
  /*SVN_ERR(svn_branch_txn__get_debug(&wc->edit_txn, wc->edit_txn, scratch_pool));*/

  edit_root_branch_id = wc->working->branch_id;
  edit_root_branch = svn_branch_txn_get_branch_by_id(
                       commit_txn, wc->working->branch_id, scratch_pool);

  /* We might be creating a new top-level branch in this commit. That is the
     only case in which the working branch will not be found in EDIT_TXN.
     (Creating any other branch can only be done inside a checkout of a
     parent branch.) So, maybe create a new top-level branch. */
  if (! edit_root_branch)
    {
      /* Create a new top-level branch in the edited state. (It will have
         an independent new top-level branch number.) */
      svn_branch_rev_bid_eid_t *from
        = svn_branch_rev_bid_eid_create(wc->base->revision, wc->base->branch_id,
                                        svn_branch_root_eid(wc->base->branch),
                                        scratch_pool);

      SVN_ERR(do_topbranch(&edit_root_branch, commit_txn,
                           from, scratch_pool, scratch_pool));
      edit_root_branch_id = edit_root_branch->bid;
    }
  /* Allocate all the new eids we'll need in this new txn */
  SVN_ERR(allocate_eids(commit_txn, wc->working->branch->txn, scratch_pool));
  SVN_ERR(replay(commit_txn, edit_root_branch,
                 wc->base->branch,
                 wc->working->branch,
                 scratch_pool));
  SVN_ERR(txn_is_changed(commit_txn, &change_detected,
                         scratch_pool));
  if (change_detected)
    {
      ccbb.edit_txn = commit_txn;
      ccbb.wc_base_branch_id = wc->base->branch_id;
      ccbb.wc_commit_branch_id = edit_root_branch_id;

      SVN_ERR(svn_branch_txn_complete(commit_txn, scratch_pool));
      SVN_ERR(display_diff_of_commit(&ccbb, scratch_pool));

      wc->head_revision = ccbb.revision;
      if (new_rev_p)
        *new_rev_p = ccbb.revision;
    }
  else
    {
      SVN_ERR(svn_branch_txn_abort(commit_txn, scratch_pool));
      if (new_rev_p)
        *new_rev_p = SVN_INVALID_REVNUM;
    }

  wc->list_of_commands = NULL;

  return SVN_NO_ERROR;
}

typedef enum action_code_t {
  ACTION_INFO_WC,
  ACTION_DIFF,
  ACTION_LOG,
  ACTION_LIST_BRANCHES,
  ACTION_LIST_BRANCHES_R,
  ACTION_LS,
  ACTION_TBRANCH,
  ACTION_BRANCH,
  ACTION_BRANCH_INTO,
  ACTION_MKBRANCH,
  ACTION_MERGE,
  ACTION_MV,
  ACTION_MKDIR,
  ACTION_PUT_FILE,
  ACTION_CAT,
  ACTION_CP,
  ACTION_RM,
  ACTION_CP_RM,
  ACTION_BR_RM,
  ACTION_BR_INTO_RM,
  ACTION_COMMIT,
  ACTION_UPDATE,
  ACTION_SWITCH,
  ACTION_STATUS,
  ACTION_REVERT,
  ACTION_MIGRATE
} action_code_t;

typedef struct action_defn_t {
  enum action_code_t code;
  const char *name;
  int num_args;
  const char *args_help;
  const char *help;
} action_defn_t;

#define NL "\n                           "
static const action_defn_t action_defn[] =
{
  {ACTION_INFO_WC,          "info-wc", 0, "",
    "print information about the WC"},
  {ACTION_LIST_BRANCHES,    "branches", 1, "PATH",
    "list all branches rooted at the same element as PATH"},
  {ACTION_LIST_BRANCHES_R,  "ls-br-r", 0, "",
    "list all branches, recursively"},
  {ACTION_LS,               "ls", 1, "PATH",
    "list elements in the branch found at PATH"},
  {ACTION_LOG,              "log", 2, "FROM@REV TO@REV",
    "show per-revision diffs between FROM and TO"},
  {ACTION_TBRANCH,          "tbranch", 1, "SRC",
    "branch the branch-root or branch-subtree at SRC" NL
    "to make a new top-level branch"},
  {ACTION_BRANCH,           "branch", 2, "SRC DST",
    "branch the branch-root or branch-subtree at SRC" NL
    "to make a new branch at DST"},
  {ACTION_BRANCH_INTO,      "branch-into", 2, "SRC DST",
    "make a branch of the existing subtree SRC appear at" NL
    "DST as part of the existing branch that contains DST" NL
    "(like merging the creation of SRC to DST)"},
  {ACTION_MKBRANCH,         "mkbranch", 1, "ROOT",
    "make a directory that's the root of a new subbranch"},
  {ACTION_DIFF,             "diff", 2, "LEFT@REV RIGHT@REV",
    "show differences from subtree LEFT to subtree RIGHT"},
  {ACTION_MERGE,            "merge", 3, "FROM TO YCA@REV",
    "3-way merge YCA->FROM into TO"},
  {ACTION_CP,               "cp", 2, "REV SRC DST",
    "copy SRC@REV to DST"},
  {ACTION_MV,               "mv", 2, "SRC DST",
    "move SRC to DST"},
  {ACTION_RM,               "rm", 1, "PATH",
    "delete PATH"},
  {ACTION_CP_RM,            "copy-and-delete", 2, "SRC DST",
    "copy-and-delete SRC to DST"},
  {ACTION_BR_RM,            "branch-and-delete", 2, "SRC DST",
    "branch-and-delete SRC to DST"},
  {ACTION_BR_INTO_RM,       "branch-into-and-delete", 2, "SRC DST",
    "merge-and-delete SRC to DST"},
  {ACTION_MKDIR,            "mkdir", 1, "PATH",
    "create new directory PATH"},
  {ACTION_PUT_FILE,         "put", 2, "LOCAL_FILE PATH",
    "add or modify file PATH with text copied from" NL
    "LOCAL_FILE (use \"-\" to read from standard input)"},
  {ACTION_CAT,              "cat", 1, "PATH",
    "display text (for a file) and props (if any) of PATH"},
  {ACTION_COMMIT,           "commit", 0, "",
    "commit the changes"},
  {ACTION_UPDATE,           "update", 1, ".@REV",
    "update to revision REV, keeping local changes"},
  {ACTION_SWITCH,           "switch", 1, "TARGET[@REV]",
    "switch to another branch and/or revision, keeping local changes"},
  {ACTION_STATUS,           "status", 0, "",
    "same as 'diff .@base .'"},
  {ACTION_REVERT,           "revert", 0, "",
    "revert all uncommitted changes"},
  {ACTION_MIGRATE,          "migrate", 1, ".@REV",
    "migrate changes from non-move-tracking revision"},
};

typedef struct action_t {
  /* The original command words (const char *) by which the action was
     specified */
  apr_array_header_t *action_args;

  action_code_t action;

  /* argument revisions */
  svn_opt_revision_t rev_spec[3];

  const char *branch_id[3];

  /* argument paths */
  const char *relpath[3];
} action_t;

/* ====================================================================== */

/* Find the deepest branch in the repository of which REVNUM:BRANCH_ID:RELPATH
 * is either the root element or a normal, non-sub-branch element.
 *
 * RELPATH is a repository-relative path. REVNUM is a revision number, or
 * SVN_INVALID_REVNUM meaning the current txn.
 *
 * Return the location of the element in that branch, or with
 * EID=-1 if no element exists there.
 *
 * If BRANCH_ID is null, the default is the WC base branch when REVNUM is
 * specified, and the WC working branch when REVNUM is SVN_INVALID_REVNUM.
 *
 * Return an error if branch BRANCH_ID does not exist in r<REVNUM>; otherwise,
 * the result will never be NULL, as every path is within at least the root
 * branch.
 */
static svn_error_t *
find_el_rev_by_rrpath_rev(svn_branch_el_rev_id_t **el_rev_p,
                          svnmover_wc_t *wc,
                          svn_revnum_t revnum,
                          const char *branch_id,
                          const char *relpath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  if (SVN_IS_VALID_REVNUM(revnum))
    {
      const svn_branch_repos_t *repos = wc->working->branch->txn->repos;

      if (! branch_id)
        branch_id = wc->base->branch_id;
      SVN_ERR(svn_branch_repos_find_el_rev_by_path_rev(el_rev_p, repos,
                                                       revnum,
                                                       branch_id,
                                                       relpath,
                                                       result_pool,
                                                       scratch_pool));
    }
  else
    {
      svn_branch_state_t *branch
        = branch_id ? svn_branch_txn_get_branch_by_id(
                        wc->working->branch->txn, branch_id, scratch_pool)
                    : wc->working->branch;
      svn_branch_el_rev_id_t *el_rev = apr_palloc(result_pool, sizeof(*el_rev));

      if (! branch)
        return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                                 _("Branch %s not found in working state"),
                                 branch_id);
      svn_branch_find_nested_branch_element_by_relpath(
        &el_rev->branch, &el_rev->eid,
        branch, relpath, scratch_pool);
      el_rev->rev = SVN_INVALID_REVNUM;
      *el_rev_p = el_rev;
    }
  SVN_ERR_ASSERT(*el_rev_p);
  return SVN_NO_ERROR;
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that it is a subbranch root element for SUBBRANCH.
 * Return "" if SUBBRANCH is null.
 */
static const char *
branch_str(svn_branch_state_t *subbranch,
           apr_pool_t *result_pool)
{
  if (subbranch)
    return apr_psprintf(result_pool,
                      " (branch %s)",
                      svn_branch_get_id(subbranch, result_pool));
  return "";
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that BRANCH:EID is a subbranch root element.
 * Return "" if the element is not a subbranch root element.
 */
static const char *
subbranch_str(svn_branch_state_t *branch,
              int eid,
              apr_pool_t *result_pool)
{
  svn_branch_state_t *subbranch
    = svn_branch_get_subbranch_at_eid(branch, eid, result_pool);

  return branch_str(subbranch, result_pool);
}

/*  */
static const char *
subtree_subbranch_str(svn_branch_subtree_t *subtree,
                      const char *bid,
                      int eid,
                      apr_pool_t *result_pool)
{
  svn_branch_subtree_t *subbranch
    = svn_branch_subtree_get_subbranch_at_eid(subtree, eid, result_pool);

  if (subbranch)
    return apr_psprintf(result_pool,
                        " (branch %s)",
                        svn_branch_id_nest(bid, eid, result_pool));
  return "";
}

/*  */
static const char *
el_rev_id_to_path(svn_branch_el_rev_id_t *el_rev,
                  apr_pool_t *result_pool)
{
  const char *path
    = svn_branch_get_rrpath_by_eid(el_rev->branch, el_rev->eid, result_pool);

  return path;
}

/*  */
static const char *
branch_peid_name_to_path(svn_branch_state_t *to_branch,
                         int to_parent_eid,
                         const char *to_name,
                         apr_pool_t *result_pool)
{
  const char *path
    = svn_relpath_join(svn_branch_get_rrpath_by_eid(to_branch, to_parent_eid,
                                                    result_pool),
                       to_name, result_pool);

  return path;
}

/* List the elements in BRANCH, in path notation.
 *
 * List only the elements for which a relpath is known -- that is, elements
 * whose parents exist all the way up to the branch root.
 */
static svn_error_t *
list_branch_elements(svn_branch_state_t *branch,
                     apr_pool_t *scratch_pool)
{
  apr_hash_t *paths_to_eid = apr_hash_make(scratch_pool);
  apr_hash_index_t *hi;
  SVN_ITER_T(int) *pi;

  for (hi = apr_hash_first(scratch_pool, svn_branch_get_elements(branch));
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      const char *relpath = svn_branch_get_path_by_eid(branch, eid,
                                                       scratch_pool);

      if (relpath)
        {
          svn_hash_sets(paths_to_eid, relpath, apr_pmemdup(scratch_pool,
                                                           &eid, sizeof(eid)));
        }
    }
  for (SVN_HASH_ITER_SORTED(pi, paths_to_eid, svn_sort_compare_items_as_paths, scratch_pool))
    {
      const char *relpath = pi->key;
      int eid = *pi->val;

      notify("    %-20s%s",
             relpath[0] ? relpath : ".",
             subbranch_str(branch, eid, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/*  */
static int
sort_compare_items_by_eid(const svn_sort__item_t *a,
                          const svn_sort__item_t *b)
{
  int eid_a = *(const int *)a->key;
  int eid_b = *(const int *)b->key;

  return eid_a - eid_b;
}

static const char *
peid_name(const svn_element_content_t *element,
          apr_pool_t *scratch_pool)
{
  if (element->parent_eid == -1)
    return apr_psprintf(scratch_pool, "%3s %-10s", "", ".");

  return apr_psprintf(scratch_pool, "%3d/%-10s",
                      element->parent_eid, element->name);
}

static const char elements_by_eid_header[]
  = "    eid  parent-eid/name\n"
    "    ---  ----------/----";

/* List all elements in branch BRANCH, in element notation.
 */
static svn_error_t *
list_branch_elements_by_eid(svn_branch_state_t *branch,
                            apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_element_content_t) *pi;

  notify_v("%s", elements_by_eid_header);
  for (SVN_HASH_ITER_SORTED(pi, svn_branch_get_elements(branch),
                            sort_compare_items_by_eid, scratch_pool))
    {
      int eid = *(const int *)(pi->key);
      svn_element_content_t *element = pi->val;

      if (element)
        {
          notify("    e%-3d %21s%s",
                 eid,
                 peid_name(element, scratch_pool),
                 subbranch_str(branch, eid, scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}

/*  */
static const char *
branch_id_header_str(const char *prefix,
                     apr_pool_t *result_pool)
{
  if (the_ui_mode == UI_MODE_PATHS)
    {
      return apr_psprintf(result_pool,
                          "%sbranch-id  root-path\n"
                          "%s---------  ---------",
                          prefix, prefix);
    }
  else
    {
      return apr_psprintf(result_pool,
                          "%sbranch-id  branch-name  root-eid\n"
                          "%s---------  -----------  --------",
                          prefix, prefix);
    }
}

/* Show the id and path or root-eid of BRANCH.
 */
static const char *
branch_id_str(svn_branch_state_t *branch,
              apr_pool_t *result_pool)
{
  apr_pool_t *scratch_pool = result_pool;

  if (the_ui_mode == UI_MODE_PATHS)
    {
      return apr_psprintf(result_pool, "%-10s /%s",
                          svn_branch_get_id(branch, scratch_pool),
                          svn_branch_get_root_rrpath(branch, scratch_pool));
    }
  else
    {
      svn_element_content_t *outer_el = NULL;
      svn_branch_state_t *outer_branch;
      int outer_eid;

      svn_branch_get_outer_branch_and_eid(&outer_branch, &outer_eid,
                                          branch, scratch_pool);

      if (outer_branch)
        outer_el = svn_branch_get_element(outer_branch, outer_eid);

      return apr_psprintf(result_pool, "%-10s %-12s root=e%d",
                          svn_branch_get_id(branch, scratch_pool),
                          outer_el ? outer_el->name : "/",
                          svn_branch_root_eid(branch));
    }
}

/* List the branch BRANCH.
 *
 * If WITH_ELEMENTS is true, also list the elements in it.
 */
static svn_error_t *
list_branch(svn_branch_state_t *branch,
            svn_boolean_t with_elements,
            apr_pool_t *scratch_pool)
{
  notify_v("  %s", branch_id_str(branch, scratch_pool));

  if (with_elements)
    {
      if (the_ui_mode == UI_MODE_PATHS)
        {
          SVN_ERR(list_branch_elements(branch, scratch_pool));
        }
      else
        {
          SVN_ERR(list_branch_elements_by_eid(branch, scratch_pool));
        }
    }
  return SVN_NO_ERROR;
}

/* List all branches rooted at EID.
 *
 * If WITH_ELEMENTS is true, also list the elements in each branch.
 */
static svn_error_t *
list_branches(svn_branch_txn_t *txn,
              int eid,
              svn_boolean_t with_elements,
              apr_pool_t *scratch_pool)
{
  const apr_array_header_t *branches;
  SVN_ITER_T(svn_branch_state_t) *bi;
  svn_boolean_t printed_header = FALSE;

  notify_v("%s", branch_id_header_str("  ", scratch_pool));

  branches = svn_branch_txn_get_branches(txn, scratch_pool);

  for (SVN_ARRAY_ITER(bi, branches, scratch_pool))
    {
      svn_branch_state_t *branch = bi->val;

      if (svn_branch_root_eid(branch) != eid)
        continue;

      SVN_ERR(list_branch(branch, with_elements, bi->iterpool));
      if (with_elements) /* separate branches by a blank line */
        notify("");
    }

  for (SVN_ARRAY_ITER(bi, branches, scratch_pool))
    {
      svn_branch_state_t *branch = bi->val;

      if (! svn_branch_get_element(branch, eid)
          || svn_branch_root_eid(branch) == eid)
        continue;

      if (! printed_header)
        {
          if (the_ui_mode == UI_MODE_PATHS)
            notify_v("branches containing but not rooted at that element:");
          else
            notify_v("branches containing but not rooted at e%d:", eid);
          printed_header = TRUE;
        }
      SVN_ERR(list_branch(branch, with_elements, bi->iterpool));
      if (with_elements) /* separate branches by a blank line */
        notify("");
    }

  return SVN_NO_ERROR;
}

/* List all branches. If WITH_ELEMENTS is true, also list the elements
 * in each branch.
 */
static svn_error_t *
list_all_branches(svn_branch_txn_t *txn,
                  svn_boolean_t with_elements,
                  apr_pool_t *scratch_pool)
{
  const apr_array_header_t *branches;
  SVN_ITER_T(svn_branch_state_t) *bi;

  branches = svn_branch_txn_get_branches(txn, scratch_pool);

  notify_v("branches:");

  for (SVN_ARRAY_ITER(bi, branches, scratch_pool))
    {
      svn_branch_state_t *branch = bi->val;

      SVN_ERR(list_branch(branch, with_elements, bi->iterpool));
      if (with_elements) /* separate branches by a blank line */
        notify("");
    }

  return SVN_NO_ERROR;
}

/* Switch the WC to revision REVISION (SVN_INVALID_REVNUM means HEAD)
 * and branch TARGET_BRANCH.
 *
 * Merge any changes in the existing txn into the new txn.
 */
static svn_error_t *
do_switch(svnmover_wc_t *wc,
          svn_revnum_t revision,
          svn_branch_state_t *target_branch,
          apr_pool_t *scratch_pool)
{
  const char *target_branch_id
    = svn_branch_get_id(target_branch, scratch_pool);
  /* Keep hold of the previous WC txn */
  svn_branch_state_t *previous_base_br = wc->base->branch;
  svn_branch_state_t *previous_working_br = wc->working->branch;
  svn_boolean_t has_local_changes;

  SVN_ERR(txn_is_changed(previous_working_br->txn,
                         &has_local_changes, scratch_pool));

  /* Usually one would switch the WC to another branch (or just another
     revision) rooted at the same element. Switching to a branch rooted
     at a different element is well defined, but give a warning. */
  if (has_local_changes
      && svn_branch_root_eid(target_branch)
         != svn_branch_root_eid(previous_base_br))
    {
      notify(_("Warning: you are switching from %s rooted at e%d "
               "to %s rooted at e%d, a different root element, "
               "while there are local changes. "),
             svn_branch_get_id(previous_base_br, scratch_pool),
             svn_branch_root_eid(previous_base_br),
             target_branch_id,
             svn_branch_root_eid(target_branch));
    }

  /* Complete the old edit drive into the 'WC' txn */
  SVN_ERR(svn_branch_txn_sequence_point(wc->edit_txn, scratch_pool));

  /* Check out a new WC, re-using the same data object */
  SVN_ERR(wc_checkout(wc, revision, target_branch_id, scratch_pool));

  if (has_local_changes)
    {
      svn_branch_el_rev_id_t *yca, *src, *tgt;
      conflict_storage_t *conflicts;

      /* Merge changes from the old into the new WC */
      yca = svn_branch_el_rev_id_create(previous_base_br,
                                        svn_branch_root_eid(previous_base_br),
                                        previous_base_br->txn->rev,
                                        scratch_pool);
      src = svn_branch_el_rev_id_create(previous_working_br,
                                        svn_branch_root_eid(previous_working_br),
                                        SVN_INVALID_REVNUM, scratch_pool);
      tgt = svn_branch_el_rev_id_create(wc->working->branch,
                                        svn_branch_root_eid(wc->working->branch),
                                        SVN_INVALID_REVNUM, scratch_pool);
      SVN_ERR(svnmover_branch_merge(wc->edit_txn, &conflicts,
                                    src, tgt, yca, scratch_pool));

      if (apr_hash_count(conflicts->single_element_conflicts)
          || apr_hash_count(conflicts->name_clash_conflicts)
          || apr_hash_count(conflicts->orphan_conflicts))
        {
          SVN_ERR(svnmover_display_conflicts(conflicts, "switch: ",
                                             scratch_pool));
          return svn_error_createf(
                   SVN_ERR_BRANCHING, NULL,
                   _("Switch failed because of conflicts: "
                     "%d single-element conflicts, "
                     "%d name-clash conflicts, "
                     "%d orphan conflicts"),
                   apr_hash_count(conflicts->single_element_conflicts),
                   apr_hash_count(conflicts->name_clash_conflicts),
                   apr_hash_count(conflicts->orphan_conflicts));
        }
      else
        {
          SVN_DBG(("Switch completed: no conflicts"));
        }

      /* ### TODO: If the merge raises conflicts, either revert to the
             pre-update state or store and handle the conflicts. Currently
             this just leaves the merge partially done and raises an error. */
    }

  return SVN_NO_ERROR;
}

/*  */
typedef struct diff_item_t
{
  int eid;
  svn_element_content_t *e0, *e1;
  const char *relpath0, *relpath1;
  svn_boolean_t modified, reparented, renamed;
} diff_item_t;

/* Return differences between branch subtrees S_LEFT and S_RIGHT.
 *
 * Set *DIFF_CHANGES to a hash of (eid -> diff_item_t).
 *
 * ### This requires 'subtrees' only in order to produce the 'relpath'
 *     fields in the output. Other than that, it would work with arbitrary
 *     sets of elements.
 */
static svn_error_t *
subtree_diff(apr_hash_t **diff_changes,
             svn_branch_subtree_t *s_left,
             svn_branch_subtree_t *s_right,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_left_right;
  apr_hash_index_t *hi;

  *diff_changes = apr_hash_make(result_pool);

  SVN_ERR(element_differences(&diff_left_right,
                              s_left->tree, s_right->tree,
                              result_pool, scratch_pool));

  for (hi = apr_hash_first(scratch_pool, diff_left_right);
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      svn_element_content_t **e_pair = apr_hash_this_val(hi);
      svn_element_content_t *e0 = e_pair[0], *e1 = e_pair[1];

      if (e0 || e1)
        {
          diff_item_t *item = apr_palloc(result_pool, sizeof(*item));

          item->eid = eid;
          item->e0 = e0;
          item->e1 = e1;
          item->relpath0 = e0 ? svn_element_tree_get_path_by_eid(
                                  s_left->tree, eid, result_pool) : NULL;
          item->relpath1 = e1 ? svn_element_tree_get_path_by_eid(
                                  s_right->tree, eid, result_pool) : NULL;
          item->reparented = (e0 && e1 && e0->parent_eid != e1->parent_eid);
          item->renamed = (e0 && e1 && strcmp(e0->name, e1->name) != 0);

          svn_int_hash_set(*diff_changes, eid, item);
        }
    }

  return SVN_NO_ERROR;
}

/* Find the relative order of diff items A and B, according to the
 * "major path" of each. The major path means its right-hand relpath, if
 * it exists on the right-hand side of the diff, else its left-hand relpath.
 *
 * Return negative/zero/positive when A sorts before/equal-to/after B.
 */
static int
diff_ordering_major_paths(const struct svn_sort__item_t *a,
                          const struct svn_sort__item_t *b)
{
  const diff_item_t *item_a = a->value, *item_b = b->value;
  int deleted_a = (item_a->e0 && ! item_a->e1);
  int deleted_b = (item_b->e0 && ! item_b->e1);
  const char *major_path_a = (item_a->e1 ? item_a->relpath1 : item_a->relpath0);
  const char *major_path_b = (item_b->e1 ? item_b->relpath1 : item_b->relpath0);

  /* Sort deleted items before all others */
  if (deleted_a != deleted_b)
    return deleted_b - deleted_a;

  /* Sort by path */
  return svn_path_compare_paths(major_path_a, major_path_b);
}

/* Display differences between subtrees LEFT and RIGHT, which are subtrees
 * of branches LEFT_BID and RIGHT_BID respectively.
 *
 * Use EDITOR to fetch content when needed.
 *
 * Write a line containing HEADER before any other output, if it is not
 * null. Write PREFIX at the start of each line of output, including any
 * header line. PREFIX and HEADER should contain no end-of-line characters.
 *
 * The output refers to paths or to elements according to THE_UI_MODE.
 */
static svn_error_t *
show_subtree_diff(svn_branch_subtree_t *left,
                  const char *left_bid,
                  svn_branch_subtree_t *right,
                  const char *right_bid,
                  const char *prefix,
                  const char *header,
                  apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_changes;
  SVN_ITER_T(diff_item_t) *ai;

  SVN_ERR_ASSERT(left && left->tree->root_eid >= 0
                 && right && right->tree->root_eid >= 0);

  SVN_ERR(subtree_diff(&diff_changes, left, right,
                       scratch_pool, scratch_pool));

  if (header && apr_hash_count(diff_changes))
    notify("%s%s", prefix, header);

  for (SVN_HASH_ITER_SORTED(ai, diff_changes,
                            (the_ui_mode == UI_MODE_EIDS)
                              ? sort_compare_items_by_eid
                              : diff_ordering_major_paths,
                            scratch_pool))
    {
      diff_item_t *item = ai->val;
      svn_element_content_t *e0 = item->e0, *e1 = item->e1;
      char status_mod = (e0 && e1) ? 'M' : e0 ? 'D' : 'A';

      /* For a deleted element whose parent was also deleted, mark it is
         less interesting, somehow. (Or we could omit it entirely.) */
      if (status_mod == 'D')
        {
          diff_item_t *parent_item
            = svn_int_hash_get(diff_changes, e0->parent_eid);

          if (parent_item && ! parent_item->e1)
            status_mod = 'd';
        }

      if (the_ui_mode == UI_MODE_PATHS)
        {
          const char *major_path = (e1 ? item->relpath1 : item->relpath0);
          const char *from = "";

          if (item->reparented || item->renamed)
            {
              if (! item->reparented)
                from = apr_psprintf(scratch_pool,
                                    " (renamed from .../%s)",
                                    e0->name);
              else if (! item->renamed)
                from = apr_psprintf(scratch_pool,
                                    " (moved from %s/...)",
                                    svn_relpath_dirname(item->relpath0,
                                                        scratch_pool));
              else
                from = apr_psprintf(scratch_pool,
                                    " (moved+renamed from %s)",
                                    item->relpath0);
            }
          notify("%s%c%c%c %s%s%s",
                 prefix,
                 status_mod,
                 item->reparented ? 'v' : ' ', item->renamed ? 'r' : ' ',
                 major_path,
                 subtree_subbranch_str(e0 ? left : right,
                                       e0 ? left_bid : right_bid,
                                       item->eid, scratch_pool),
                 from);
        }
      else
        {
          notify("%s%c%c%c e%-3d  %s%s%s%s%s",
                 prefix,
                 status_mod,
                 item->reparented ? 'v' : ' ', item->renamed ? 'r' : ' ',
                 item->eid,
                 e1 ? peid_name(e1, scratch_pool) : "",
                 subtree_subbranch_str(e0 ? left : right,
                                       e0 ? left_bid : right_bid,
                                       item->eid, scratch_pool),
                 e0 && e1 ? " (from " : "",
                 e0 ? peid_name(e0, scratch_pool) : "",
                 e0 && e1 ? ")" : "");
        }
    }

  return SVN_NO_ERROR;
}

typedef svn_error_t *
svn_branch_diff_func_t(svn_branch_subtree_t *left,
                       const char *left_bid,
                       svn_branch_subtree_t *right,
                       const char *right_bid,
                       const char *prefix,
                       const char *header,
                       apr_pool_t *scratch_pool);

/* Display differences between subtrees LEFT and RIGHT.
 *
 * Recurse into sub-branches.
 */
static svn_error_t *
subtree_diff_r(svn_branch_subtree_t *left,
               svn_revnum_t left_rev,
               const char *left_bid,
               const char *left_rrpath,
               svn_branch_subtree_t *right,
               svn_revnum_t right_rev,
               const char *right_bid,
               const char *right_rrpath,
               svn_branch_diff_func_t diff_func,
               const char *prefix,
               apr_pool_t *scratch_pool)
{
  const char *left_str
    = left ? apr_psprintf(scratch_pool, "r%ld:%s:e%d at /%s",
                          left_rev, left_bid, left->tree->root_eid, left_rrpath)
           : NULL;
  const char *right_str
    = right ? apr_psprintf(scratch_pool, "r%ld:%s:e%d at /%s",
                           right_rev, right_bid, right->tree->root_eid, right_rrpath)
            : NULL;
  const char *header;
  apr_hash_t *subbranches_l, *subbranches_r, *subbranches_all;
  apr_hash_index_t *hi;

  SVN_DBG(("subtree_diff_r: l='%s' r='%s'",
           left ? left_rrpath : "<nil>",
           right ? right_rrpath : "<nil>"));

  if (!left)
    {
      header = apr_psprintf(scratch_pool,
                 "--- added branch %s",
                 right_str);
      notify("%s%s", prefix, header);
    }
  else if (!right)
    {
      header = apr_psprintf(scratch_pool,
                 "--- deleted branch %s",
                 left_str);
      notify("%s%s", prefix, header);
    }
  else
    {
      if (strcmp(left_str, right_str) == 0)
        {
          header = apr_psprintf(
                     scratch_pool, "--- diff branch %s",
                     left_str);
        }
      else
        {
          header = apr_psprintf(
                     scratch_pool, "--- diff branch %s : %s",
                     left_str, right_str);
        }
      SVN_ERR(diff_func(left, left_bid, right, right_bid,
                        prefix, header,
                        scratch_pool));
    }

  /* recurse into each subbranch that exists in LEFT and/or in RIGHT */
  subbranches_l = left ? left->subbranches : apr_hash_make(scratch_pool);
  subbranches_r = right ? right->subbranches : apr_hash_make(scratch_pool);
  subbranches_all = apr_hash_overlay(scratch_pool,
                                     subbranches_l, subbranches_r);

  for (hi = apr_hash_first(scratch_pool, subbranches_all);
       hi; hi = apr_hash_next(hi))
    {
      int e = svn_int_hash_this_key(hi);
      svn_branch_subtree_t *sub_left = NULL, *sub_right = NULL;
      const char *sub_left_bid = NULL, *sub_right_bid = NULL;
      const char *sub_left_rrpath = NULL, *sub_right_rrpath = NULL;

      /* recurse */
      if (left)
        {
          sub_left = svn_branch_subtree_get_subbranch_at_eid(left, e,
                                                             scratch_pool);
          if (sub_left)
            {
              const char *relpath
                = svn_element_tree_get_path_by_eid(left->tree, e, scratch_pool);

              sub_left_bid = svn_branch_id_nest(left_bid, e, scratch_pool);
              sub_left_rrpath = svn_relpath_join(left_rrpath, relpath,
                                                 scratch_pool);
            }
        }
      if (right)
        {
          sub_right = svn_branch_subtree_get_subbranch_at_eid(right, e,
                                                              scratch_pool);
          if (sub_right)
            {
              const char *relpath
                = svn_element_tree_get_path_by_eid(right->tree, e, scratch_pool);

              sub_right_bid = svn_branch_id_nest(right_bid, e, scratch_pool);
              sub_right_rrpath = svn_relpath_join(right_rrpath, relpath,
                                                  scratch_pool);
            }
        }
      SVN_ERR(subtree_diff_r(sub_left, left_rev, sub_left_bid, sub_left_rrpath,
                             sub_right, right_rev, sub_right_bid, sub_right_rrpath,
                             diff_func, prefix, scratch_pool));
    }
  return SVN_NO_ERROR;
}

/* Display differences between branch subtrees LEFT and RIGHT.
 *
 * Recurse into sub-branches.
 */
static svn_error_t *
branch_diff_r(svn_branch_el_rev_id_t *left,
              svn_branch_el_rev_id_t *right,
              svn_branch_diff_func_t diff_func,
              const char *prefix,
              apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t *s_left
    = svn_branch_get_subtree(left->branch, left->eid, scratch_pool);
  svn_branch_subtree_t *s_right
    = svn_branch_get_subtree(right->branch, right->eid, scratch_pool);

  SVN_ERR(subtree_diff_r(s_left,
                         left->rev,
                         svn_branch_get_id(left->branch, scratch_pool),
                         svn_branch_get_root_rrpath(left->branch, scratch_pool),
                         s_right,
                         right->rev,
                         svn_branch_get_id(right->branch, scratch_pool),
                         svn_branch_get_root_rrpath(right->branch, scratch_pool),
                         diff_func, prefix, scratch_pool));
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
do_copy(svn_branch_el_rev_id_t *from_el_rev,
        svn_branch_state_t *to_branch,
        svn_branch_eid_t to_parent_eid,
        const char *new_name,
        apr_pool_t *scratch_pool)
{
  const char *from_branch_id = svn_branch_get_id(from_el_rev->branch,
                                                 scratch_pool);
  svn_branch_rev_bid_eid_t *src_el_rev
    = svn_branch_rev_bid_eid_create(from_el_rev->rev, from_branch_id,
                                    from_el_rev->eid, scratch_pool);

  SVN_ERR(svn_branch_state_copy_tree(to_branch,
                                     src_el_rev, to_parent_eid, new_name,
                                     scratch_pool));
  notify_v("A+   %s (from %s)",
           branch_peid_name_to_path(to_branch, to_parent_eid, new_name,
                                    scratch_pool),
           el_rev_id_to_path(from_el_rev, scratch_pool));

  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
do_delete(svn_branch_state_t *branch,
          svn_branch_eid_t eid,
          apr_pool_t *scratch_pool)
{
  const char *path = svn_branch_get_rrpath_by_eid(branch, eid, scratch_pool);

  SVN_ERR(svn_branch_state_delete_one(branch, eid, scratch_pool));
  notify_v("D    %s", path);
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
do_mkdir(svn_branch_txn_t *txn,
         svn_branch_state_t *to_branch,
         svn_branch_eid_t to_parent_eid,
         const char *new_name,
         apr_pool_t *scratch_pool)
{
  apr_hash_t *props = apr_hash_make(scratch_pool);
  svn_element_payload_t *payload
    = svn_element_payload_create_dir(props, scratch_pool);
  int new_eid;

  SVN_ERR(svn_branch_txn_new_eid(txn, &new_eid, scratch_pool));
  SVN_ERR(svn_branch_state_alter_one(to_branch, new_eid,
                                     to_parent_eid, new_name, payload,
                                     scratch_pool));
  notify_v("A    %s",
           branch_peid_name_to_path(to_branch, to_parent_eid, new_name,
                                    scratch_pool));
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
do_put_file(svn_branch_txn_t *txn,
            const char *local_file_path,
            svn_branch_el_rev_id_t *file_el_rev,
            svn_branch_el_rev_id_t *parent_el_rev,
            const char *file_name,
            apr_pool_t *scratch_pool)
{
  apr_hash_t *props;
  svn_stringbuf_t *text;
  int parent_eid;
  const char *name;
  svn_element_payload_t *payload;

  if (file_el_rev->eid >= 0)
    {
      /* get existing props */
      svn_element_content_t *existing_element
        = svn_branch_get_element(file_el_rev->branch, file_el_rev->eid);

      props = existing_element->payload->props;
    }
  else
    {
      props = apr_hash_make(scratch_pool);
    }
  /* read new text from file */
  {
    svn_stream_t *src;

    if (strcmp(local_file_path, "-") != 0)
      SVN_ERR(svn_stream_open_readonly(&src, local_file_path,
                                       scratch_pool, scratch_pool));
    else
      SVN_ERR(svn_stream_for_stdin(&src, scratch_pool));

    svn_stringbuf_from_stream(&text, src, 0, scratch_pool);
  }
  payload = svn_element_payload_create_file(props, text, scratch_pool);

  if (is_branch_root_element(file_el_rev->branch,
                             file_el_rev->eid))
    {
      parent_eid = -1;
      name = "";
    }
  else
    {
      parent_eid = parent_el_rev->eid;
      name = file_name;
    }

  if (file_el_rev->eid >= 0)
    {
      SVN_ERR(svn_branch_state_alter_one(file_el_rev->branch, file_el_rev->eid,
                                         parent_eid, name, payload,
                                         scratch_pool));
      notify_v("M    %s",
               el_rev_id_to_path(file_el_rev, scratch_pool));
    }
  else
    {
      int new_eid;

      SVN_ERR(svn_branch_txn_new_eid(txn, &new_eid, scratch_pool));
      SVN_ERR(svn_branch_state_alter_one(parent_el_rev->branch, new_eid,
                                         parent_eid, name, payload,
                                         scratch_pool));
      file_el_rev->eid = new_eid;
      notify_v("A    %s",
               el_rev_id_to_path(file_el_rev, scratch_pool));
    }
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
do_cat(svn_branch_el_rev_id_t *file_el_rev,
       apr_pool_t *scratch_pool)
{
  apr_hash_t *props;
  svn_stringbuf_t *text;
  apr_hash_index_t *hi;

  /* get existing props */
  svn_element_content_t *existing_element
    = svn_branch_get_element(file_el_rev->branch, file_el_rev->eid);

  props = existing_element->payload->props;
  text = existing_element->payload->text;

  for (hi = apr_hash_first(scratch_pool, props); hi; hi = apr_hash_next(hi))
    {
      const char *pname = apr_hash_this_key(hi);
      svn_string_t *pval = apr_hash_this_val(hi);

      notify("property '%s': '%s'", pname, pval->data);
    }
  if (text)
    {
      notify("%s", text->data);
    }
  return SVN_NO_ERROR;
}

/* Set *NEW_EL_REV_P to the location where OLD_EL_REV was in the previous
 * revision. Branching is followed.
 *
 * If the same EID...
 */
static svn_error_t *
svn_branch_find_predecessor_el_rev(svn_branch_el_rev_id_t **new_el_rev_p,
                                   svn_branch_el_rev_id_t *old_el_rev,
                                   apr_pool_t *result_pool)
{
  const svn_branch_repos_t *repos = old_el_rev->branch->txn->repos;
  svn_branch_rev_bid_t *predecessor = old_el_rev->branch->predecessor;
  svn_branch_state_t *branch;

  if (! predecessor)
    {
      *new_el_rev_p = NULL;
      return SVN_NO_ERROR;
    }

  /* A predecessor can point at another branch within the same revision.
     We don't want that result, so iterate until we find another revision. */
  while (predecessor->rev == old_el_rev->rev)
    {
      branch = svn_branch_txn_get_branch_by_id(
                 old_el_rev->branch->txn, predecessor->bid, result_pool);
      predecessor = branch->predecessor;
    }

  SVN_ERR(svn_branch_repos_get_branch_by_id(&branch,
                                            repos, predecessor->rev,
                                            predecessor->bid, result_pool));
  *new_el_rev_p = svn_branch_el_rev_id_create(branch, old_el_rev->eid,
                                              predecessor->rev, result_pool);

  return SVN_NO_ERROR;
}

/* Similar to 'svn log -v', this iterates over the revisions between
 * LEFT and RIGHT (currently excluding LEFT), printing a single-rev diff
 * for each.
 */
static svn_error_t *
svn_branch_log(svn_branch_el_rev_id_t *left,
               svn_branch_el_rev_id_t *right,
               apr_pool_t *scratch_pool)
{
  svn_revnum_t first_rev = left->rev;

  while (right->rev > first_rev)
    {
      svn_branch_el_rev_id_t *el_rev_left;

      SVN_ERR(svn_branch_find_predecessor_el_rev(&el_rev_left, right, scratch_pool));

      notify(SVN_CL__LOG_SEP_STRING "r%ld | ...",
             right->rev);
      notify("Changed elements:");
      SVN_ERR(branch_diff_r(el_rev_left, right,
                            show_subtree_diff, "   ",
                            scratch_pool));
      right = el_rev_left;
    }

  return SVN_NO_ERROR;
}

/* Make a subbranch at OUTER_BRANCH : OUTER_PARENT_EID : OUTER_NAME.
 *
 * The subbranch will consist of a single element given by PAYLOAD.
 */
static svn_error_t *
mk_branch(const char **new_branch_id_p,
          svn_branch_txn_t *txn,
          svn_branch_state_t *outer_branch,
          int outer_parent_eid,
          const char *outer_name,
          svn_element_payload_t *payload,
          apr_pool_t *scratch_pool)
{
  const char *outer_branch_id = svn_branch_get_id(outer_branch, scratch_pool);
  int new_outer_eid, new_inner_eid;
  const char *new_branch_id;
  svn_branch_state_t *new_branch;

  SVN_ERR(svn_branch_txn_new_eid(txn, &new_outer_eid, scratch_pool));
  SVN_ERR(svn_branch_state_alter_one(outer_branch, new_outer_eid,
                                     outer_parent_eid, outer_name,
                                     svn_element_payload_create_subbranch(
                                       scratch_pool), scratch_pool));

  SVN_ERR(svn_branch_txn_new_eid(txn, &new_inner_eid, scratch_pool));
  new_branch_id = svn_branch_id_nest(outer_branch_id, new_outer_eid,
                                     scratch_pool);
  SVN_ERR(svn_branch_txn_open_branch(txn, &new_branch,
                                     NULL /*predecessor*/, new_branch_id,
                                     new_inner_eid, scratch_pool, scratch_pool));
  SVN_ERR(svn_branch_state_alter_one(new_branch, new_inner_eid,
                                     -1, "", payload, scratch_pool));

  notify_v("A    %s (branch %s)",
           svn_branch_get_path_by_eid(outer_branch, new_outer_eid,
                                      scratch_pool),
           new_branch->bid);
  if (new_branch_id_p)
    *new_branch_id_p = new_branch->bid;
  return SVN_NO_ERROR;
}

/* Branch all or part of an existing branch, making a new branch.
 *
 * Branch the subtree of FROM_BRANCH found at FROM_EID, to create
 * a new branch at TO_OUTER_BRANCH:TO_OUTER_PARENT_EID:NEW_NAME.
 *
 * FROM_BRANCH:FROM_EID must be an existing element. It may be the
 * root of FROM_BRANCH. It must not be the root of a subbranch of
 * FROM_BRANCH.
 *
 * TO_OUTER_BRANCH:TO_OUTER_PARENT_EID must be an existing directory
 * and NEW_NAME must be nonexistent in that directory.
 */
static svn_error_t *
do_branch(svn_branch_state_t **new_branch_p,
          svn_branch_txn_t *txn,
          svn_branch_rev_bid_eid_t *from,
          svn_branch_state_t *to_outer_branch,
          svn_branch_eid_t to_outer_parent_eid,
          const char *new_name,
          apr_pool_t *result_pool,
          apr_pool_t *scratch_pool)
{
  const char *to_outer_branch_id
    = to_outer_branch ? svn_branch_get_id(to_outer_branch, scratch_pool) : NULL;
  int to_outer_eid;
  const char *new_branch_id;
  svn_branch_state_t *new_branch;

  /* assign new eid to root element (outer branch) */
  SVN_ERR(svn_branch_txn_new_eid(txn, &to_outer_eid, scratch_pool));

  new_branch_id = svn_branch_id_nest(to_outer_branch_id, to_outer_eid,
                                     scratch_pool);
  SVN_ERR(svn_branch_txn_branch(txn, &new_branch,
                                from, new_branch_id,
                                result_pool, scratch_pool));
  SVN_ERR(svn_branch_state_alter_one(to_outer_branch, to_outer_eid,
                                     to_outer_parent_eid, new_name,
                                     svn_element_payload_create_subbranch(
                                       scratch_pool), scratch_pool));

  notify_v("A+   %s (branch %s)",
           svn_branch_get_path_by_eid(to_outer_branch, to_outer_eid,
                                      scratch_pool),
           new_branch->bid);

  if (new_branch_p)
    *new_branch_p = new_branch;
  return SVN_NO_ERROR;
}

static svn_error_t *
do_topbranch(svn_branch_state_t **new_branch_p,
             svn_branch_txn_t *txn,
             svn_branch_rev_bid_eid_t *from,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  int outer_eid;
  const char *new_branch_id;
  svn_branch_state_t *new_branch;

  SVN_ERR(svn_branch_txn_new_eid(txn, &outer_eid, scratch_pool));
  new_branch_id = svn_branch_id_nest(NULL /*outer_branch*/, outer_eid,
                                     scratch_pool);
  SVN_ERR(svn_branch_txn_branch(txn, &new_branch,
                                from, new_branch_id,
                                result_pool, scratch_pool));

  notify_v("A+   (branch %s)",
           new_branch->bid);

  if (new_branch_p)
    *new_branch_p = new_branch;
  return SVN_NO_ERROR;
}

/* Branch the subtree of FROM_BRANCH found at FROM_EID, to appear
 * in the existing branch TO_BRANCH at TO_PARENT_EID:NEW_NAME.
 *
 * This is like merging the creation of the source subtree into TO_BRANCH.
 *
 * Any elements of the source subtree that already exist in TO_BRANCH
 * are altered. This is like resolving any merge conflicts as 'theirs'.
 *
 * (### Sometimes the user might prefer that we throw an error if any
 * element of the source subtree already exists in TO_BRANCH.)
 */
static svn_error_t *
do_branch_into(svn_branch_state_t *from_branch,
               int from_eid,
               svn_branch_state_t *to_branch,
               svn_branch_eid_t to_parent_eid,
               const char *new_name,
               apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t *from_subtree;
  svn_element_content_t *new_root_content;

  /* Source element must exist */
  if (! svn_branch_get_path_by_eid(from_branch, from_eid, scratch_pool))
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("Cannot branch from %s e%d: "
                                 "does not exist"),
                               svn_branch_get_id(
                                 from_branch, scratch_pool), from_eid);
    }

  from_subtree = svn_branch_get_subtree(from_branch, from_eid, scratch_pool);

  /* Change this subtree's root element to TO_PARENT_EID/NEW_NAME. */
  new_root_content
    = svn_element_tree_get(from_subtree->tree, from_subtree->tree->root_eid);
  new_root_content
    = svn_element_content_create(to_parent_eid, new_name,
                                 new_root_content->payload, scratch_pool);
  svn_element_tree_set(from_subtree->tree, from_subtree->tree->root_eid,
                         new_root_content);

  /* Populate the new branch mapping */
  SVN_ERR(svn_branch_instantiate_elements_r(to_branch, *from_subtree,
                                            scratch_pool));
  notify_v("A+   %s (subtree)",
           svn_branch_get_path_by_eid(to_branch, from_eid, scratch_pool));

  return SVN_NO_ERROR;
}

/* Copy-and-delete.
 *
 *      copy the subtree at EL_REV to TO_BRANCH:TO_PARENT_EID:TO_NAME
 *      delete the subtree at EL_REV
 */
static svn_error_t *
do_copy_and_delete(svn_branch_el_rev_id_t *el_rev,
                   svn_branch_state_t *to_branch,
                   int to_parent_eid,
                   const char *to_name,
                   apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(! is_branch_root_element(el_rev->branch, el_rev->eid));

  SVN_ERR(do_copy(el_rev, to_branch, to_parent_eid, to_name,
                  scratch_pool));
  SVN_ERR(do_delete(el_rev->branch, el_rev->eid, scratch_pool));
  return SVN_NO_ERROR;
}

/* Branch-and-delete.
 *
 *      branch the subtree at EL_REV creating a new nested branch at
 *        TO_BRANCH:TO_PARENT_EID:TO_NAME,
 *        or creating a new top-level branch if TO_BRANCH is null;
 *      delete the subtree at EL_REV
 */
static svn_error_t *
do_branch_and_delete(svn_branch_txn_t *edit_txn,
                     svn_branch_el_rev_id_t *el_rev,
                     svn_branch_state_t *to_outer_branch,
                     int to_outer_parent_eid,
                     const char *to_name,
                     apr_pool_t *scratch_pool)
{
  const char *from_branch_id = svn_branch_get_id(el_rev->branch,
                                                 scratch_pool);
  svn_branch_rev_bid_eid_t *from
    = svn_branch_rev_bid_eid_create(el_rev->rev, from_branch_id,
                                    el_rev->eid, scratch_pool);
  svn_branch_state_t *new_branch;

  SVN_ERR_ASSERT(! is_branch_root_element(el_rev->branch, el_rev->eid));

  SVN_ERR(do_branch(&new_branch, edit_txn, from,
                    to_outer_branch, to_outer_parent_eid, to_name,
                    scratch_pool, scratch_pool));

  SVN_ERR(do_delete(el_rev->branch, el_rev->eid, scratch_pool));

  return SVN_NO_ERROR;
}

/* Branch-into-and-delete.
 *
 * (Previously, confusingly, called 'branch-and-delete'.)
 *
 * The target branch is different from the source branch.
 *
 *      delete elements from source branch
 *      instantiate (or update) same elements in target branch
 *
 * For each element being moved, if the element already exists in TO_BRANCH,
 * the effect is as if the existing element in TO_BRANCH was first deleted.
 */
static svn_error_t *
do_branch_into_and_delete(svn_branch_el_rev_id_t *el_rev,
                          svn_branch_state_t *to_branch,
                          int to_parent_eid,
                          const char *to_name,
                          apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(! is_branch_root_element(el_rev->branch, el_rev->eid));

  /* This is supposed to be used for moving to a *different* branch.
     In fact, this method would also work for moving within one
     branch, but we don't currently want to use it for that purpose. */
  SVN_ERR_ASSERT(! BRANCH_IS_SAME_BRANCH(el_rev->branch, to_branch,
                                         scratch_pool));

  /* Merge the "creation of the source" to the target (aka branch-into) */
  SVN_ERR(do_branch_into(el_rev->branch, el_rev->eid,
                         to_branch, to_parent_eid, to_name,
                         scratch_pool));

  SVN_ERR(do_delete(el_rev->branch, el_rev->eid, scratch_pool));

  return SVN_NO_ERROR;
}

/* Interactive options for moving to another branch.
 */
static svn_error_t *
do_interactive_cross_branch_move(svn_branch_txn_t *txn,
                                 svn_branch_el_rev_id_t *el_rev,
                                 svn_branch_el_rev_id_t *to_parent_el_rev,
                                 const char *to_name,
                                 apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  const char *input;

  if (0 /*### if non-interactive*/)
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
        _("mv: The source and target are in different branches. "
          "Some ways to move content to a different branch are, "
          "depending on the effect you want to achieve: "
          "copy-and-delete, branch-and-delete, branch-into-and-delete"));
    }

  notify_v(
    _("mv: The source and target are in different branches. "
      "Some ways to move content to a different branch are, "
      "depending on the effect you want to achieve:\n"
      "  c: copy-and-delete: cp SOURCE TARGET; rm SOURCE\n"
      "  b: branch-and-delete: branch SOURCE TARGET; rm SOURCE\n"
      "  i: branch-into-and-delete: branch-into SOURCE TARGET; rm SOURCE\n"
      "We can do one of these for you now if you wish.\n"
    ));

  err = svn_cmdline_prompt_user2(
          &input,
          "Your choice (c, b, i, or just <enter> to do nothing): ",
          NULL, scratch_pool);
  if (err && (err->apr_err == SVN_ERR_CANCELLED || err->apr_err == APR_EOF))
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);

  if (input[0] == 'c' || input[0] == 'C')
    {
      notify_v("Performing 'copy-and-delete SOURCE TARGET'");

      SVN_ERR(do_copy_and_delete(el_rev,
                                 to_parent_el_rev->branch,
                                 to_parent_el_rev->eid, to_name,
                                 scratch_pool));
    }
  else if (input[0] == 'b' || input[0] == 'B')
    {
      notify_v("Performing 'branch-and-delete SOURCE TARGET'");

      SVN_ERR(do_branch_and_delete(txn, el_rev,
                                   to_parent_el_rev->branch,
                                   to_parent_el_rev->eid, to_name,
                                   scratch_pool));
    }
  else if (input[0] == 'i' || input[0] == 'I')
    {
      notify_v("Performing 'branch-into-and-delete SOURCE TARGET'");
      notify_v(
        "In the current implementation of this experimental UI, each element "
        "instance from the source branch subtree will overwrite any instance "
        "of the same element that already exists in the target branch."
        );
      /* We could instead either throw an error or fall back to copy-and-delete
         if any moved element already exists in target branch. */

      SVN_ERR(do_branch_into_and_delete(el_rev,
                                        to_parent_el_rev->branch,
                                        to_parent_el_rev->eid, to_name,
                                        scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Move.
 */
static svn_error_t *
do_move(svn_branch_el_rev_id_t *el_rev,
        svn_branch_el_rev_id_t *to_parent_el_rev,
        const char *to_name,
        apr_pool_t *scratch_pool)
{
  const char *from_path = el_rev_id_to_path(el_rev, scratch_pool);
  /* New payload shall be the same as before */
  svn_element_content_t *existing_element
    = svn_branch_get_element(el_rev->branch, el_rev->eid);

  SVN_ERR(svn_branch_state_alter_one(el_rev->branch, el_rev->eid,
                            to_parent_el_rev->eid, to_name,
                            existing_element->payload, scratch_pool));
  notify_v("V    %s (from %s)",
           branch_peid_name_to_path(to_parent_el_rev->branch,
                                    to_parent_el_rev->eid, to_name,
                                    scratch_pool),
           from_path);
  return SVN_NO_ERROR;
}

/* This commit callback prints not only a commit summary line but also
 * a log-style summary of the changes.
 */
static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool)
{
  commit_callback_baton_t *b = baton;

  notify("Committed r%ld:", commit_info->revision);

  b->revision = commit_info->revision;
  return SVN_NO_ERROR;
}

/* Display a diff of the commit */
static svn_error_t *
display_diff_of_commit(const commit_callback_baton_t *ccbb,
                       apr_pool_t *scratch_pool)
{
  svn_branch_txn_t *previous_head_txn
    = svn_branch_repos_get_base_revision_root(ccbb->edit_txn);
  svn_branch_state_t *base_branch
    = svn_branch_txn_get_branch_by_id(previous_head_txn,
                                      ccbb->wc_base_branch_id,
                                      scratch_pool);
  svn_branch_state_t *committed_branch
    = svn_branch_txn_get_branch_by_id(ccbb->edit_txn,
                                      ccbb->wc_commit_branch_id,
                                      scratch_pool);
  svn_branch_el_rev_id_t *el_rev_left
    = svn_branch_el_rev_id_create(base_branch, svn_branch_root_eid(base_branch),
                                  base_branch->txn->rev,
                                  scratch_pool);
  svn_branch_el_rev_id_t *el_rev_right
    = svn_branch_el_rev_id_create(committed_branch,
                                  svn_branch_root_eid(committed_branch),
                                  committed_branch->txn->rev,
                                  scratch_pool);

  SVN_ERR(branch_diff_r(el_rev_left, el_rev_right,
                        show_subtree_diff, "   ",
                        scratch_pool));
  return SVN_NO_ERROR;
}

/* Commit and update WC.
 *
 * Set *NEW_REV_P to the committed revision number, and update the WC to
 * that revision.
 *
 * If there are no changes to commit, set *NEW_REV_P to SVN_INVALID_REVNUM
 * and do not make a commit and do not update the WC.
 *
 * NEW_REV_P may be null if not wanted.
 */
static svn_error_t *
do_commit(svn_revnum_t *new_rev_p,
          svnmover_wc_t *wc,
          apr_hash_t *revprops,
          apr_pool_t *scratch_pool)
{
  svn_revnum_t new_rev;

  /* Complete the old edit drive (into the 'WC') */
  SVN_ERR(svn_branch_txn_sequence_point(wc->edit_txn, scratch_pool));

  /* Commit */
  SVN_ERR(wc_commit(&new_rev, wc, revprops, scratch_pool));

  /* Check out a new WC.
     (Instead, we could perhaps just get a new WC editor.) */
  SVN_ERR(wc_checkout(wc, SVN_IS_VALID_REVNUM(new_rev) ? new_rev
                                                       : wc->base->revision,
                      wc->working->branch_id, scratch_pool));

  if (new_rev_p)
    *new_rev_p = new_rev;
  return SVN_NO_ERROR;
}

/* Revert all uncommitted changes in WC.
 */
static svn_error_t *
do_revert(svnmover_wc_t *wc,
          apr_pool_t *scratch_pool)
{
  /* Replay the inverse of the current edit txn, into the current edit txn */
  SVN_ERR(replay(wc->edit_txn, wc->working->branch,
                 wc->working->branch,
                 wc->base->branch,
                 scratch_pool));

  return SVN_NO_ERROR;
}

/* Migration replay baton */
typedef struct migrate_replay_baton_t {
  svn_branch_txn_t *edit_txn;
  svn_ra_session_t *from_session;
  /* Hash (by revnum) of array of svn_repos_move_info_t. */
  apr_hash_t *moves;
} migrate_replay_baton_t;

/* Callback function for svn_ra_replay_range, invoked when starting to parse
 * a replay report.
 */
static svn_error_t *
migrate_replay_rev_started(svn_revnum_t revision,
                           void *replay_baton,
                           const svn_delta_editor_t **editor,
                           void **edit_baton,
                           apr_hash_t *rev_props,
                           apr_pool_t *pool)
{
  migrate_replay_baton_t *rb = replay_baton;
  const svn_delta_editor_t *old_editor;
  void *old_edit_baton;

  SVN_DBG(("migrate: start r%ld", revision));

  SVN_ERR(svn_branch_get_migration_editor(&old_editor, &old_edit_baton,
                                          rb->edit_txn,
                                          rb->from_session, revision,
                                          pool));
  SVN_ERR(svn_delta__get_debug_editor(&old_editor, &old_edit_baton,
                                      old_editor, old_edit_baton,
                                      "migrate: ", pool));

  *editor = old_editor;
  *edit_baton = old_edit_baton;

  return SVN_NO_ERROR;
}

/* Callback function for svn_ra_replay_range, invoked when finishing parsing
 * a replay report.
 */
static svn_error_t *
migrate_replay_rev_finished(svn_revnum_t revision,
                            void *replay_baton,
                            const svn_delta_editor_t *editor,
                            void *edit_baton,
                            apr_hash_t *rev_props,
                            apr_pool_t *pool)
{
  migrate_replay_baton_t *rb = replay_baton;
  apr_array_header_t *moves_in_revision
    = apr_hash_get(rb->moves, &revision, sizeof(revision));

  SVN_ERR(editor->close_edit(edit_baton, pool));

  SVN_DBG(("migrate: moves in revision r%ld:", revision));

  if (moves_in_revision)
    {
      int i;

      for (i = 0; i < moves_in_revision->nelts; i++)
        {
          svn_repos_move_info_t *this_move
            = APR_ARRAY_IDX(moves_in_revision, i, void *);

          if (this_move)
            {
              notify("%s",
                     svn_client__format_move_chain_for_display(this_move,
                                                               "", pool));
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Migrate changes from non-move-tracking revisions.
 */
static svn_error_t *
do_migrate(svnmover_wc_t *wc,
           svn_revnum_t start_revision,
           svn_revnum_t end_revision,
           apr_pool_t *scratch_pool)
{
  migrate_replay_baton_t *rb = apr_pcalloc(scratch_pool, sizeof(*rb));

  if (start_revision < 1 || end_revision < 1
      || start_revision > end_revision
      || end_revision > wc->head_revision)
    {
      return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                               _("migrate: Bad revision range (%ld to %ld); "
                                 "minimum is 1 and maximum (head) is %ld"),
                               start_revision, end_revision,
                               wc->head_revision);
    }

  /* Scan the repository log for move info */
  SVN_ERR(svn_client__get_repos_moves(&rb->moves,
                                      "" /*(unused)*/,
                                      wc->ra_session,
                                      start_revision, end_revision,
                                      wc->ctx, scratch_pool, scratch_pool));

  rb->edit_txn = wc->edit_txn;
  rb->from_session = wc->ra_session;
  SVN_ERR(svn_ra_replay_range(rb->from_session,
                              start_revision, end_revision,
                              0, TRUE,
                              migrate_replay_rev_started,
                              migrate_replay_rev_finished,
                              rb, scratch_pool));
  return SVN_NO_ERROR;
}


typedef struct arg_t
{
  const char *path_name;
  svn_revnum_t revnum;
  svn_branch_el_rev_id_t *el_rev, *parent_el_rev;
} arg_t;

#define VERIFY_REV_SPECIFIED(op, i)                                     \
  if (arg[i]->el_rev->rev == SVN_INVALID_REVNUM)                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: '%s': revision number required"),   \
                             op, action->relpath[i]);

#define VERIFY_REV_UNSPECIFIED(op, i)                                   \
  if (arg[i]->el_rev->rev != SVN_INVALID_REVNUM)                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: '%s@...': revision number not allowed"), \
                             op, action->relpath[i]);

#define VERIFY_EID_NONEXISTENT(op, i)                                   \
  if (arg[i]->el_rev->eid != -1)                                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Element already exists at path '%s'"), \
                             op, action->relpath[i]);

#define VERIFY_EID_EXISTS(op, i)                                        \
  if (arg[i]->el_rev->eid == -1)                                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Element not found at path '%s%s'"), \
                             op, action->relpath[i],                    \
                             action->rev_spec[i].kind == svn_opt_revision_unspecified \
                               ? "" : "@...");

#define VERIFY_PARENT_EID_EXISTS(op, i)                                 \
  if (arg[i]->parent_el_rev->eid == -1)                                 \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Element not found at path '%s'"),   \
                             op, svn_relpath_dirname(action->relpath[i], pool));

#define VERIFY_NOT_CHILD_OF_SELF(op, i, j, pool)                        \
  if (svn_relpath_skip_ancestor(                                        \
        svn_branch_get_rrpath_by_eid(arg[i]->el_rev->branch,            \
                                     arg[i]->el_rev->eid, pool),        \
        svn_branch_get_rrpath_by_eid(arg[j]->parent_el_rev->branch,     \
                                     arg[j]->parent_el_rev->eid, pool))) \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: The specified target is nested "    \
                               "inside the source"), op);

/* If EL_REV specifies the root element of a nested branch, change EL_REV
 * to specify the corresponding subbranch-root element of its outer branch.
 *
 * If EL_REV specifies the root element of a top-level branch, return an
 * error.
 */
static svn_error_t *
point_to_outer_element_instead(svn_branch_el_rev_id_t *el_rev,
                               const char *op,
                               apr_pool_t *scratch_pool)
{
  if (is_branch_root_element(el_rev->branch, el_rev->eid))
    {
      svn_branch_state_t *outer_branch;
      int outer_eid;

      svn_branch_get_outer_branch_and_eid(&outer_branch, &outer_eid,
                                          el_rev->branch, scratch_pool);

      if (! outer_branch)
        return svn_error_createf(SVN_ERR_BRANCHING, NULL, "%s: %s", op,
                                 _("svnmover cannot delete or move a "
                                   "top-level branch"));

      el_rev->eid = outer_eid;
      el_rev->branch = outer_branch;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
execute(svnmover_wc_t *wc,
        const apr_array_header_t *actions,
        const char *anchor_url,
        apr_hash_t *revprops,
        svn_client_ctx_t *ctx,
        apr_pool_t *pool)
{
  const char *base_relpath;
  /* This pool is passed to svn_branch_merge() and needs to be a
     subpool of the pool used to allocate the e_map members of the
     data passed to the function.  The pool relationship is required
     by apr_hash_overlay() to guarantee the lifetime of the resulting
     hash. */
  apr_pool_t *iterpool = svn_pool_create(wc->pool);
  int i;

  base_relpath = svn_uri_skip_ancestor(wc->repos_root_url, anchor_url, pool);

  for (i = 0; i < actions->nelts; ++i)
    {
      action_t *action = APR_ARRAY_IDX(actions, i, action_t *);
      int j;
      arg_t *arg[3] = { NULL, NULL, NULL };

      svn_pool_clear(iterpool);

      /* Before translating paths to/from elements, need a sequence point */
      SVN_ERR(svn_branch_txn_sequence_point(wc->edit_txn, iterpool));

      /* Convert each ACTION[j].{relpath, rev_spec} to
         (EL_REV[j], PARENT_EL_REV[j], PATH_NAME[j], REVNUM[j]),
         except for the local-path argument of a 'put' command. */
      for (j = 0; j < 3; j++)
        {
          if (action->relpath[j]
              && ! (action->action == ACTION_PUT_FILE && j == 0))
            {
              const char *rrpath, *parent_rrpath;

              arg[j] = apr_palloc(iterpool, sizeof(*arg[j]));
              if (action->rev_spec[j].kind == svn_opt_revision_unspecified)
                arg[j]->revnum = SVN_INVALID_REVNUM;
              else if (action->rev_spec[j].kind == svn_opt_revision_number)
                arg[j]->revnum = action->rev_spec[j].value.number;
              else if (action->rev_spec[j].kind == svn_opt_revision_head)
                {
                  arg[j]->revnum = wc->head_revision;
                }
              else if (action->rev_spec[j].kind == svn_opt_revision_base
                       || action->rev_spec[j].kind == svn_opt_revision_committed)
                {
                  arg[j]->revnum = wc->base->revision;
                }
              else
                return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                         "'%s@...': revision specifier "
                                         "must be a number or 'head', 'base' "
                                         "or 'committed'",
                                         action->relpath[j]);

              rrpath = svn_relpath_join(base_relpath, action->relpath[j], iterpool);
              parent_rrpath = svn_relpath_dirname(rrpath, iterpool);

              arg[j]->path_name = svn_relpath_basename(rrpath, NULL);
              SVN_ERR(find_el_rev_by_rrpath_rev(&arg[j]->el_rev, wc,
                                                arg[j]->revnum,
                                                action->branch_id[j],
                                                rrpath,
                                                iterpool, iterpool));
              SVN_ERR(find_el_rev_by_rrpath_rev(&arg[j]->parent_el_rev, wc,
                                                arg[j]->revnum,
                                                action->branch_id[j],
                                                parent_rrpath,
                                                iterpool, iterpool));
            }
        }

      switch (action->action)
        {
        case ACTION_INFO_WC:
          notify("Repository Root: %s", wc->repos_root_url);
          notify("Base Revision: %ld", wc->base->revision);
          notify("Base Branch:    %s", wc->base->branch_id);
          notify("Working Branch: %s", wc->working->branch_id);
          break;

        case ACTION_DIFF:
          VERIFY_EID_EXISTS("diff", 0);
          VERIFY_EID_EXISTS("diff", 1);
          {
            SVN_ERR(branch_diff_r(arg[0]->el_rev /*from*/,
                                  arg[1]->el_rev /*to*/,
                                  show_subtree_diff, "",
                                  iterpool));
          }
          break;

        case ACTION_STATUS:
          {
            svn_branch_el_rev_id_t *from, *to;

            from = svn_branch_el_rev_id_create(wc->base->branch,
                                               svn_branch_root_eid(wc->base->branch),
                                               wc->base->revision, iterpool);
            to = svn_branch_el_rev_id_create(wc->working->branch,
                                             svn_branch_root_eid(wc->working->branch),
                                             SVN_INVALID_REVNUM, iterpool);
            SVN_ERR(branch_diff_r(from, to,
                                  show_subtree_diff, "",
                                  iterpool));
          }
          break;

        case ACTION_LOG:
          VERIFY_EID_EXISTS("log", 0);
          VERIFY_EID_EXISTS("log", 1);
          {
            SVN_ERR(svn_branch_log(arg[0]->el_rev /*from*/,
                                   arg[1]->el_rev /*to*/,
                                   iterpool));
          }
          break;

        case ACTION_LIST_BRANCHES:
          {
            VERIFY_EID_EXISTS("branches", 0);
            if (the_ui_mode == UI_MODE_PATHS)
              {
                notify_v("branches rooted at same element as '%s':",
                       action->relpath[0]);
              }
            else
              {
                notify_v("branches rooted at e%d:", arg[0]->el_rev->eid);
              }
            SVN_ERR(list_branches(
                      arg[0]->el_rev->branch->txn,
                      arg[0]->el_rev->eid,
                      FALSE, iterpool));
          }
          break;

        case ACTION_LIST_BRANCHES_R:
          {
            if (the_ui_mode == UI_MODE_SERIAL)
              {
                svn_stream_t *stream;
                SVN_ERR(svn_stream_for_stdout(&stream, iterpool));
                SVN_ERR(svn_branch_txn_serialize(wc->working->branch->txn,
                          stream,
                          iterpool));
              }
            else
              {
                /* Note: BASE_REVISION is always a real revision number, here */
                SVN_ERR(list_all_branches(wc->working->branch->txn, TRUE,
                                          iterpool));
              }
          }
          break;

        case ACTION_LS:
          {
            VERIFY_EID_EXISTS("ls", 0);
            if (the_ui_mode == UI_MODE_PATHS)
              {
                SVN_ERR(list_branch_elements(arg[0]->el_rev->branch, iterpool));
              }
            else if (the_ui_mode == UI_MODE_EIDS)
              {
                SVN_ERR(list_branch_elements_by_eid(arg[0]->el_rev->branch,
                                                    iterpool));
              }
            else
              {
                svn_stream_t *stream;
                SVN_ERR(svn_stream_for_stdout(&stream, iterpool));
                SVN_ERR(svn_branch_state_serialize(stream,
                                                   arg[0]->el_rev->branch,
                                                   iterpool));
              }
          }
          break;

        case ACTION_TBRANCH:
          VERIFY_EID_EXISTS("tbranch", 0);
          {
            const char *from_branch_id = svn_branch_get_id(arg[0]->el_rev->branch,
                                                           iterpool);
            svn_branch_rev_bid_eid_t *from
              = svn_branch_rev_bid_eid_create(arg[0]->el_rev->rev, from_branch_id,
                                              arg[0]->el_rev->eid, iterpool);
            svn_branch_state_t *new_branch;

            SVN_ERR(do_topbranch(&new_branch, wc->edit_txn,
                                 from,
                                 iterpool, iterpool));
            /* Switch the WC working state to this new branch */
            wc->working->branch_id = new_branch->bid;
            wc->working->branch = new_branch;
          }
          break;

        case ACTION_BRANCH:
          VERIFY_EID_EXISTS("branch", 0);
          VERIFY_REV_UNSPECIFIED("branch", 1);
          VERIFY_EID_NONEXISTENT("branch", 1);
          VERIFY_PARENT_EID_EXISTS("branch", 1);
          {
            const char *from_branch_id = svn_branch_get_id(arg[0]->el_rev->branch,
                                                           iterpool);
            svn_branch_rev_bid_eid_t *from
              = svn_branch_rev_bid_eid_create(arg[0]->el_rev->rev, from_branch_id,
                                              arg[0]->el_rev->eid, iterpool);
            svn_branch_state_t *new_branch;

            SVN_ERR(do_branch(&new_branch, wc->edit_txn,
                              from,
                              arg[1]->el_rev->branch, arg[1]->parent_el_rev->eid,
                              arg[1]->path_name,
                              iterpool, iterpool));
          }
          break;

        case ACTION_BRANCH_INTO:
          VERIFY_EID_EXISTS("branch-into", 0);
          VERIFY_REV_UNSPECIFIED("branch-into", 1);
          VERIFY_EID_NONEXISTENT("branch-into", 1);
          VERIFY_PARENT_EID_EXISTS("branch-into", 1);
          {
            SVN_ERR(do_branch_into(arg[0]->el_rev->branch, arg[0]->el_rev->eid,
                                   arg[1]->el_rev->branch,
                                   arg[1]->parent_el_rev->eid, arg[1]->path_name,
                                   iterpool));
          }
          break;

        case ACTION_MKBRANCH:
          VERIFY_REV_UNSPECIFIED("mkbranch", 0);
          VERIFY_EID_NONEXISTENT("mkbranch", 0);
          VERIFY_PARENT_EID_EXISTS("mkbranch", 0);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_element_payload_t *payload
              = svn_element_payload_create_dir(props, iterpool);

            SVN_ERR(mk_branch(NULL, wc->edit_txn,
                              arg[0]->parent_el_rev->branch,
                              arg[0]->parent_el_rev->eid, arg[0]->path_name,
                              payload, iterpool));
          }
          break;

        case ACTION_MERGE:
          {
            conflict_storage_t *conflicts;

            VERIFY_EID_EXISTS("merge", 0);
            VERIFY_EID_EXISTS("merge", 1);
            VERIFY_EID_EXISTS("merge", 2);
            if (arg[0]->el_rev->eid != arg[1]->el_rev->eid
                || arg[0]->el_rev->eid != arg[2]->el_rev->eid)
              {
                notify(_("Warning: root elements differ in the requested merge "
                         "(from: e%d, to: e%d, yca: e%d)"),
                       arg[0]->el_rev->eid, arg[1]->el_rev->eid, arg[2]->el_rev->eid);
              }
            SVN_ERR(svnmover_branch_merge(wc->edit_txn,
                                          &conflicts,
                                          arg[0]->el_rev /*from*/,
                                          arg[1]->el_rev /*to*/,
                                          arg[2]->el_rev /*yca*/,
                                          iterpool));

            if (apr_hash_count(conflicts->single_element_conflicts)
                || apr_hash_count(conflicts->name_clash_conflicts)
                || apr_hash_count(conflicts->orphan_conflicts))
              {
                SVN_ERR(svnmover_display_conflicts(conflicts, "merge: ",
                                                   iterpool));
                return svn_error_createf(
                         SVN_ERR_BRANCHING, NULL,
                         _("Merge failed because of conflicts: "
                           "%d single-element conflicts, "
                           "%d name-clash conflicts, "
                           "%d orphan conflicts"),
                         apr_hash_count(conflicts->single_element_conflicts),
                         apr_hash_count(conflicts->name_clash_conflicts),
                         apr_hash_count(conflicts->orphan_conflicts));
              }
            else
              {
                SVN_DBG(("Merge completed: no conflicts"));
              }

          }
          break;

        case ACTION_MV:
          SVN_ERR(point_to_outer_element_instead(arg[0]->el_rev, "mv",
                                                 iterpool));

          VERIFY_REV_UNSPECIFIED("mv", 0);
          VERIFY_EID_EXISTS("mv", 0);
          VERIFY_REV_UNSPECIFIED("mv", 1);
          VERIFY_EID_NONEXISTENT("mv", 1);
          VERIFY_PARENT_EID_EXISTS("mv", 1);
          VERIFY_NOT_CHILD_OF_SELF("mv", 0, 1, iterpool);

          /* Simple move/rename within same branch, if possible */
          if (BRANCH_IS_SAME_BRANCH(arg[1]->parent_el_rev->branch,
                                    arg[0]->el_rev->branch,
                                    iterpool))
            {
              SVN_ERR(do_move(arg[0]->el_rev,
                              arg[1]->parent_el_rev, arg[1]->path_name,
                              iterpool));
            }
          else
            {
              SVN_ERR(do_interactive_cross_branch_move(wc->edit_txn,
                                                       arg[0]->el_rev,
                                                       arg[1]->parent_el_rev,
                                                       arg[1]->path_name,
                                                       iterpool));
            }
          break;

        case ACTION_CP:
          VERIFY_REV_SPECIFIED("cp", 0);
            /* (Or do we want to support copying from "this txn" too?) */
          VERIFY_EID_EXISTS("cp", 0);
          VERIFY_REV_UNSPECIFIED("cp", 1);
          VERIFY_EID_NONEXISTENT("cp", 1);
          VERIFY_PARENT_EID_EXISTS("cp", 1);
          SVN_ERR(do_copy(arg[0]->el_rev,
                          arg[1]->parent_el_rev->branch,
                          arg[1]->parent_el_rev->eid, arg[1]->path_name,
                          iterpool));
          break;

        case ACTION_RM:
          SVN_ERR(point_to_outer_element_instead(arg[0]->el_rev, "rm",
                                                 iterpool));

          VERIFY_REV_UNSPECIFIED("rm", 0);
          VERIFY_EID_EXISTS("rm", 0);
          SVN_ERR(do_delete(arg[0]->el_rev->branch, arg[0]->el_rev->eid,
                            iterpool));
          break;

        case ACTION_CP_RM:
          SVN_ERR(point_to_outer_element_instead(arg[0]->el_rev,
                                                 "copy-and-delete", iterpool));

          VERIFY_REV_UNSPECIFIED("copy-and-delete", 0);
          VERIFY_EID_EXISTS("copy-and-delete", 0);
          VERIFY_REV_UNSPECIFIED("copy-and-delete", 1);
          VERIFY_EID_NONEXISTENT("copy-and-delete", 1);
          VERIFY_PARENT_EID_EXISTS("copy-and-delete", 1);
          VERIFY_NOT_CHILD_OF_SELF("copy-and-delete", 0, 1, iterpool);

          SVN_ERR(do_copy_and_delete(arg[0]->el_rev,
                                     arg[1]->parent_el_rev->branch,
                                     arg[1]->parent_el_rev->eid,
                                     arg[1]->path_name,
                                     iterpool));
          break;

        case ACTION_BR_RM:
          SVN_ERR(point_to_outer_element_instead(arg[0]->el_rev,
                                                 "branch-and-delete",
                                                 iterpool));

          VERIFY_REV_UNSPECIFIED("branch-and-delete", 0);
          VERIFY_EID_EXISTS("branch-and-delete", 0);
          VERIFY_REV_UNSPECIFIED("branch-and-delete", 1);
          VERIFY_EID_NONEXISTENT("branch-and-delete", 1);
          VERIFY_PARENT_EID_EXISTS("branch-and-delete", 1);
          VERIFY_NOT_CHILD_OF_SELF("branch-and-delete", 0, 1, iterpool);

          SVN_ERR(do_branch_and_delete(wc->edit_txn,
                                       arg[0]->el_rev,
                                       arg[1]->parent_el_rev->branch,
                                       arg[1]->parent_el_rev->eid,
                                       arg[1]->path_name,
                                       iterpool));
          break;

        case ACTION_BR_INTO_RM:
          SVN_ERR(point_to_outer_element_instead(arg[0]->el_rev,
                                                 "branch-into-and-delete",
                                                 iterpool));

          VERIFY_REV_UNSPECIFIED("branch-into-and-delete", 0);
          VERIFY_EID_EXISTS("branch-into-and-delete", 0);
          VERIFY_REV_UNSPECIFIED("branch-into-and-delete", 1);
          VERIFY_EID_NONEXISTENT("branch-into-and-delete", 1);
          VERIFY_PARENT_EID_EXISTS("branch-into-and-delete", 1);
          VERIFY_NOT_CHILD_OF_SELF("branch-into-and-delete", 0, 1, iterpool);

          SVN_ERR(do_branch_into_and_delete(arg[0]->el_rev,
                                            arg[1]->parent_el_rev->branch,
                                            arg[1]->parent_el_rev->eid,
                                            arg[1]->path_name,
                                            iterpool));
          break;

        case ACTION_MKDIR:
          VERIFY_REV_UNSPECIFIED("mkdir", 0);
          VERIFY_EID_NONEXISTENT("mkdir", 0);
          VERIFY_PARENT_EID_EXISTS("mkdir", 0);
          SVN_ERR(do_mkdir(wc->edit_txn,
                           arg[0]->parent_el_rev->branch,
                           arg[0]->parent_el_rev->eid, arg[0]->path_name,
                           iterpool));
          break;

        case ACTION_PUT_FILE:
          VERIFY_REV_UNSPECIFIED("put", 1);
          VERIFY_PARENT_EID_EXISTS("put", 1);
          SVN_ERR(do_put_file(wc->edit_txn,
                              action->relpath[0],
                              arg[1]->el_rev,
                              arg[1]->parent_el_rev,
                              arg[1]->path_name,
                              iterpool));
          break;

        case ACTION_CAT:
          VERIFY_EID_EXISTS("rm", 0);
          SVN_ERR(do_cat(arg[0]->el_rev,
                         iterpool));
          break;

        case ACTION_COMMIT:
          {
            svn_revnum_t new_rev;

            SVN_ERR(do_commit(&new_rev, wc, revprops, iterpool));
            if (! SVN_IS_VALID_REVNUM(new_rev))
              {
                notify_v("There are no changes to commit.");
              }
          }
          break;

        case ACTION_UPDATE:
          /* ### If current WC branch doesn't exist in target rev, should
             'update' follow to a different branch? By following merge graph?
             Presently it would try to update to a state of nonexistence. */
          /* path (or eid) is currently required for syntax, but ignored */
          VERIFY_EID_EXISTS("update", 0);
          VERIFY_REV_SPECIFIED("update", 0);
          {
            SVN_ERR(do_switch(wc, arg[0]->revnum, wc->base->branch,
                              iterpool));
          }
          break;

        case ACTION_SWITCH:
          VERIFY_EID_EXISTS("switch", 0);
          {
            SVN_ERR(do_switch(wc, arg[0]->revnum, arg[0]->el_rev->branch,
                              iterpool));
          }
          break;

        case ACTION_REVERT:
          {
            SVN_ERR(do_revert(wc, iterpool));
          }
          break;

        case ACTION_MIGRATE:
          /* path (or eid) is currently required for syntax, but ignored */
          VERIFY_EID_EXISTS("migrate", 0);
          VERIFY_REV_SPECIFIED("migrate", 0);
          {
            SVN_ERR(do_migrate(wc,
                               arg[0]->revnum, arg[0]->revnum,
                               iterpool));
          }
          break;

        default:
          SVN_ERR_MALFUNCTION();
        }

      if (action->action != ACTION_COMMIT)
        {
          wc->list_of_commands
            = apr_psprintf(pool, "%s%s\n",
                           wc->list_of_commands ? wc->list_of_commands : "",
                           svn_cstring_join(action->action_args, " ", pool));
        }
    }
  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
final_commit(svnmover_wc_t *wc,
             apr_hash_t *revprops,
             apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  /* Complete the old edit drive (into the 'WC') */
  SVN_ERR(svn_branch_txn_sequence_point(wc->edit_txn, scratch_pool));

  /* Commit, if there are any changes */
  err = wc_commit(NULL, wc, revprops, scratch_pool);

  svn_pool_destroy(wc->pool);

  return svn_error_trace(err);
}

/* Perform the typical suite of manipulations for user-provided URLs
   on URL, returning the result (allocated from POOL): IRI-to-URI
   conversion, auto-escaping, and canonicalization. */
static const char *
sanitize_url(const char *url,
             apr_pool_t *pool)
{
  url = svn_path_uri_from_iri(url, pool);
  url = svn_path_uri_autoescape(url, pool);
  return svn_uri_canonicalize(url, pool);
}

static const char *
help_for_subcommand(const action_defn_t *action, apr_pool_t *pool)
{
  const char *cmd = apr_psprintf(pool, "%s %s",
                                 action->name, action->args_help);

  return apr_psprintf(pool, "  %-22s : %s\n", cmd, action->help);
}

/* Print a usage message on STREAM, listing only the actions. */
static void
usage_actions_only(FILE *stream, apr_pool_t *pool)
{
  int i;

  for (i = 0; i < sizeof (action_defn) / sizeof (action_defn[0]); i++)
    svn_error_clear(svn_cmdline_fputs(
                      help_for_subcommand(&action_defn[i], pool),
                      stream, pool));
}

/* Print a usage message on STREAM. */
static void
usage(FILE *stream, apr_pool_t *pool)
{
  svn_error_clear(svn_cmdline_fputs(
    _("usage: svnmover -U REPO_URL [ACTION...]\n"
      "A client for experimenting with move tracking.\n"
      "\n"
      "  Commit a batch of ACTIONs to a Subversion repository, as a single\n"
      "  new revision.  With no ACTIONs specified, read actions interactively\n"
      "  from standard input, until EOF or ^C, and then commit the result.\n"
      "\n"
      "  Action arguments are of the form\n"
      "    [^B<branch-id>/]<path>[@<revnum>]\n"
      "  where\n"
      "    <branch-id> defaults to the working branch or, when <revnum> is\n"
      "                given, to the base branch\n"
      "    <path>      is a path relative to the branch\n"
      "    <revnum>    is the revision number, when making a historic reference\n"
      "\n"
      "  Move tracking metadata is stored in the repository, in on-disk files\n"
      "  for RA-local or in revprops otherwise.\n"
      "\n"
      "Actions:\n"),
                  stream, pool));
  usage_actions_only(stream, pool);
  svn_error_clear(svn_cmdline_fputs(
    _("\n"
      "Valid options:\n"
      "  --ui={eids|e|paths|p}  : display information as elements or as paths\n"
      "  -h, -? [--help]        : display this text\n"
      "  -v [--verbose]         : display debugging messages\n"
      "  -q [--quiet]           : suppress notifications\n"
      "  -m [--message] ARG     : use ARG as a log message\n"
      "  -F [--file] ARG        : read log message from file ARG\n"
      "  -u [--username] ARG    : commit the changes as username ARG\n"
      "  -p [--password] ARG    : use ARG as the password\n"
      "  -U [--root-url] ARG    : interpret all action URLs relative to ARG\n"
      "  -r [--revision] ARG    : use revision ARG as baseline for changes\n"
      "  -B [--branch-id] ARG   : work on the branch identified by ARG\n"
      "  --with-revprop ARG     : set revision property in the following format:\n"
      "                               NAME[=VALUE]\n"
      "  --non-interactive      : do no interactive prompting (default is to\n"
      "                           prompt only if standard input is a terminal)\n"
      "  --force-interactive    : do interactive prompting even if standard\n"
      "                           input is not a terminal\n"
      "  --trust-server-cert    : accept SSL server certificates from unknown\n"
      "                           certificate authorities without prompting (but\n"
      "                           only with '--non-interactive')\n"
      "  -X [--extra-args] ARG  : append arguments from file ARG (one per line;\n"
      "                           use \"-\" to read from standard input)\n"
      "  --config-dir ARG       : use ARG to override the config directory\n"
      "  --config-option ARG    : use ARG to override a configuration option\n"
      "  --no-auth-cache        : do not cache authentication tokens\n"
      "  --version              : print version information\n"),
                  stream, pool));
}

static svn_error_t *
insufficient(int i, apr_pool_t *pool)
{
  return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                           "insufficient arguments:\n"
                           "%s",
                           help_for_subcommand(&action_defn[i], pool));
}

static svn_error_t *
display_version(apr_getopt_t *os, svn_boolean_t _quiet, apr_pool_t *pool)
{
  const char *ra_desc_start
    = "The following repository access (RA) modules are available:\n\n";
  svn_stringbuf_t *version_footer;

  version_footer = svn_stringbuf_create(ra_desc_start, pool);
  SVN_ERR(svn_ra_print_modules(version_footer, pool));

  SVN_ERR(svn_opt_print_help4(NULL, "svnmover", TRUE, _quiet, FALSE,
                              version_footer->data,
                              NULL, NULL, NULL, NULL, NULL, pool));

  return SVN_NO_ERROR;
}

/* Return an error about the mutual exclusivity of the -m, -F, and
   --with-revprop=svn:log command-line options. */
static svn_error_t *
mutually_exclusive_logs_error(void)
{
  return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                          _("--message (-m), --file (-F), and "
                            "--with-revprop=svn:log are mutually "
                            "exclusive"));
}

/* Obtain the log message from multiple sources, producing an error
   if there are multiple sources. Store the result in *FINAL_MESSAGE.  */
static svn_error_t *
get_log_message(const char **final_message,
                const char *message,
                apr_hash_t *revprops,
                svn_stringbuf_t *filedata,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  svn_string_t *msg;

  *final_message = NULL;
  /* If we already have a log message in the revprop hash, then just
     make sure the user didn't try to also use -m or -F.  Otherwise,
     we need to consult -m or -F to find a log message, if any. */
  msg = svn_hash_gets(revprops, SVN_PROP_REVISION_LOG);
  if (msg)
    {
      if (filedata || message)
        return mutually_exclusive_logs_error();

      /* Remove it from the revprops; it will be re-added later */
      svn_hash_sets(revprops, SVN_PROP_REVISION_LOG, NULL);
    }
  else if (filedata)
    {
      if (message)
        return mutually_exclusive_logs_error();

      msg = svn_string_create(filedata->data, scratch_pool);
    }
  else if (message)
    {
      msg = svn_string_create(message, scratch_pool);
    }

  if (msg)
    {
      SVN_ERR_W(svn_subst_translate_string2(&msg, NULL, NULL,
                                            msg, NULL, FALSE,
                                            result_pool, scratch_pool),
                _("Error normalizing log message to internal format"));

      *final_message = msg->data;
    }

  return SVN_NO_ERROR;
}

static const char *const special_commands[] =
{
  "help",
  "--verbose",
  "--ui=paths", "--ui=eids", "--ui=serial",
};

/* Parse the action arguments into action structures. */
static svn_error_t *
parse_actions(apr_array_header_t **actions,
              apr_array_header_t *action_args,
              apr_pool_t *pool)
{
  int i;

  *actions = apr_array_make(pool, 1, sizeof(action_t *));

  for (i = 0; i < action_args->nelts; ++i)
    {
      int j, k, num_url_args;
      const char *action_string = APR_ARRAY_IDX(action_args, i, const char *);
      action_t *action = apr_pcalloc(pool, sizeof(*action));
      const char *cp_from_rev = NULL;

      /* First, parse the action. Handle some special actions immediately;
         handle normal subcommands by looking them up in the table. */
      if (! strcmp(action_string, "?") || ! strcmp(action_string, "h")
          || ! strcmp(action_string, "help"))
        {
          usage_actions_only(stdout, pool);
          return SVN_NO_ERROR;
        }
      if (! strncmp(action_string, "--ui=", 5))
        {
          SVN_ERR(svn_token__from_word_err(&the_ui_mode, ui_mode_map,
                                           action_string + 5));
          continue;
        }
      if (! strcmp(action_string, "--verbose")
          || ! strcmp(action_string, "-v"))
        {
          svn_boolean_t be_quiet = svn_dbg__quiet_mode();

          be_quiet = !be_quiet;
          svn_dbg__set_quiet_mode(be_quiet);
          notify("verbose debug messages %s", be_quiet ? "off" : "on");
          continue;
        }
      for (j = 0; j < sizeof(action_defn) / sizeof(action_defn[0]); j++)
        {
          if (strcmp(action_string, action_defn[j].name) == 0)
            {
              action->action = action_defn[j].code;
              num_url_args = action_defn[j].num_args;
              break;
            }
        }
      if (j == sizeof(action_defn) / sizeof(action_defn[0]))
        return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                 "'%s' is not an action; try 'help'.",
                                 action_string);

      action->action_args = apr_array_make(pool, 0, sizeof(const char *));
      APR_ARRAY_PUSH(action->action_args, const char *) = action_string;

      if (action->action == ACTION_CP)
        {
          /* next argument is the copy source revision */
          if (++i == action_args->nelts)
            return svn_error_trace(insufficient(j, pool));
          cp_from_rev = APR_ARRAY_IDX(action_args, i, const char *);
          APR_ARRAY_PUSH(action->action_args, const char *) = cp_from_rev;
        }

      /* Parse the required number of URLs. */
      for (k = 0; k < num_url_args; ++k)
        {
          const char *path;

          if (++i == action_args->nelts)
            return svn_error_trace(insufficient(j, pool));
          path = APR_ARRAY_IDX(action_args, i, const char *);
          APR_ARRAY_PUSH(action->action_args, const char *) = path;

          if (cp_from_rev && k == 0)
            {
              path = apr_psprintf(pool, "%s@%s", path, cp_from_rev);
            }

          SVN_ERR(svn_opt_parse_path(&action->rev_spec[k], &path, path, pool));

          /* If there's an ANCHOR_URL, we expect URL to be a path
             relative to ANCHOR_URL (and we build a full url from the
             combination of the two).  Otherwise, it should be a full
             url. */
          if (svn_path_is_url(path))
            {
              return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                       "Argument '%s' is a URL; use "
                                       "--root-url (-U) instead", path);
            }
          /* Parse "^B<branch-id>/path" syntax. */
          if (strncmp("^B", path, 2) == 0)
            {
              const char *slash = strchr(path, '/');

              action->branch_id[k]
                = slash ? apr_pstrndup(pool, path + 1, slash - (path + 1))
                        : path + 1;
              path = slash ? slash + 1 : "";
            }
          /* These args must be relpaths, except for the 'local file' arg
             of a 'put' command. */
          if (! svn_relpath_is_canonical(path)
              && ! (action->action == ACTION_PUT_FILE && k == 0))
            {
              return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                       "Argument '%s' is not a relative path "
                                       "or a URL", path);
            }
          action->relpath[k] = path;
        }

      APR_ARRAY_PUSH(*actions, action_t *) = action;
    }

  return SVN_NO_ERROR;
}

/* A command-line completion callback for the 'Line Noise' interactive
 * prompting.
 *
 * This is called when the user presses the Tab key. It calculates the
 * possible completions for the partial line BUF.
 *
 * ### So far, this only works on a single command keyword at the start
 *     of the line.
 */
static void
linenoise_completion(const char *buf, linenoiseCompletions *lc)
{
  int i;

  for (i = 0; i < sizeof(special_commands) / sizeof(special_commands[0]); i++)
    {
      /* Suggest each command that matches (and is longer than) what the
         user has already typed. Add a space. */
      if (strncmp(buf, special_commands[i], strlen(buf)) == 0
          && strlen(special_commands[i]) > strlen(buf))
        {
          static char completion[100];

          apr_cpystrn(completion, special_commands[i], 99);
          strcat(completion, " ");
          linenoiseAddCompletion(lc, completion);
        }
    }

  for (i = 0; i < sizeof(action_defn) / sizeof(action_defn[0]); i++)
    {
      /* Suggest each command that matches (and is longer than) what the
         user has already typed. Add a space. */
      if (strncmp(buf, action_defn[i].name, strlen(buf)) == 0
          && strlen(action_defn[i].name) > strlen(buf))
        {
          static char completion[100];

          apr_cpystrn(completion, action_defn[i].name, 99);
          strcat(completion, " ");
          linenoiseAddCompletion(lc, completion);
        }
    }
}

/* Display a prompt, read a line of input and split it into words.
 *
 * Set *WORDS to null if input is cancelled (by ctrl-C for example).
 */
static svn_error_t *
read_words(apr_array_header_t **words,
           const char *prompt,
           apr_pool_t *result_pool)
{
  svn_error_t *err;
  const char *input;

  err = svn_cmdline_prompt_user2(&input, prompt, NULL, result_pool);
  if (err && (err->apr_err == SVN_ERR_CANCELLED || err->apr_err == APR_EOF))
    {
      *words = NULL;
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);
  *words = svn_cstring_split(input, " ", TRUE /*chop_whitespace*/, result_pool);

  return SVN_NO_ERROR;
}

/*
 * On success, leave *EXIT_CODE untouched and return SVN_NO_ERROR. On error,
 * either return an error to be displayed, or set *EXIT_CODE to non-zero and
 * return SVN_NO_ERROR.
 */
static svn_error_t *
sub_main(int *exit_code, int argc, const char *argv[], apr_pool_t *pool)
{
  apr_array_header_t *actions;
  svn_error_t *err = SVN_NO_ERROR;
  apr_getopt_t *opts;
  enum {
    config_dir_opt = SVN_OPT_FIRST_LONGOPT_ID,
    config_inline_opt,
    no_auth_cache_opt,
    version_opt,
    with_revprop_opt,
    non_interactive_opt,
    force_interactive_opt,
    trust_server_cert_opt,
    trust_server_cert_failures_opt,
    ui_opt
  };
  static const apr_getopt_option_t options[] = {
    {"verbose", 'v', 0, ""},
    {"quiet", 'q', 0, ""},
    {"message", 'm', 1, ""},
    {"file", 'F', 1, ""},
    {"username", 'u', 1, ""},
    {"password", 'p', 1, ""},
    {"root-url", 'U', 1, ""},
    {"revision", 'r', 1, ""},
    {"branch-id", 'B', 1, ""},
    {"with-revprop",  with_revprop_opt, 1, ""},
    {"extra-args", 'X', 1, ""},
    {"help", 'h', 0, ""},
    {NULL, '?', 0, ""},
    {"non-interactive", non_interactive_opt, 0, ""},
    {"force-interactive", force_interactive_opt, 0, ""},
    {"trust-server-cert", trust_server_cert_opt, 0, ""},
    {"trust-server-cert-failures", trust_server_cert_failures_opt, 1, ""},
    {"config-dir", config_dir_opt, 1, ""},
    {"config-option",  config_inline_opt, 1, ""},
    {"no-auth-cache",  no_auth_cache_opt, 0, ""},
    {"version", version_opt, 0, ""},
    {"ui", ui_opt, 1, ""},
    {NULL, 0, 0, NULL}
  };
  const char *message = NULL;
  svn_stringbuf_t *filedata = NULL;
  const char *username = NULL, *password = NULL;
  const char *anchor_url = NULL, *extra_args_file = NULL;
  const char *config_dir = NULL;
  apr_array_header_t *config_options;
  svn_boolean_t show_version = FALSE;
  svn_boolean_t non_interactive = FALSE;
  svn_boolean_t force_interactive = FALSE;
  svn_boolean_t interactive_actions;
  svn_boolean_t trust_unknown_ca = FALSE;
  svn_boolean_t trust_cn_mismatch = FALSE;
  svn_boolean_t trust_expired = FALSE;
  svn_boolean_t trust_not_yet_valid = FALSE;
  svn_boolean_t trust_other_failure = FALSE;
  svn_boolean_t no_auth_cache = FALSE;
  svn_revnum_t base_revision = SVN_INVALID_REVNUM;
  const char *branch_id = "B0";  /* default branch */
  apr_array_header_t *action_args;
  apr_hash_t *revprops = apr_hash_make(pool);
  apr_hash_t *cfg_hash;
  svn_config_t *cfg_config;
  svn_client_ctx_t *ctx;
  const char *log_msg;
  svnmover_wc_t *wc;

  /* Check library versions */
  SVN_ERR(check_lib_versions());

  /* Suppress debug message unless '-v' given. */
  svn_dbg__set_quiet_mode(TRUE);

  config_options = apr_array_make(pool, 0,
                                  sizeof(svn_cmdline__config_argument_t*));

  apr_getopt_init(&opts, pool, argc, argv);
  opts->interleave = 1;
  while (1)
    {
      int opt;
      const char *arg;
      const char *opt_arg;

      apr_status_t status = apr_getopt_long(opts, options, &opt, &arg);
      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        return svn_error_wrap_apr(status, "getopt failure");
      switch(opt)
        {
        case 'v':
          svn_dbg__set_quiet_mode(FALSE);
          break;
        case 'q':
          quiet = TRUE;
          break;
        case 'm':
          SVN_ERR(svn_utf_cstring_to_utf8(&message, arg, pool));
          break;
        case 'F':
          {
            const char *arg_utf8;
            SVN_ERR(svn_utf_cstring_to_utf8(&arg_utf8, arg, pool));
            SVN_ERR(svn_stringbuf_from_file2(&filedata, arg, pool));
          }
          break;
        case 'u':
          username = apr_pstrdup(pool, arg);
          break;
        case 'p':
          password = apr_pstrdup(pool, arg);
          break;
        case 'U':
          SVN_ERR(svn_utf_cstring_to_utf8(&anchor_url, arg, pool));
          if (! svn_path_is_url(anchor_url))
            return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                     "'%s' is not a URL", anchor_url);
          anchor_url = sanitize_url(anchor_url, pool);
          break;
        case 'r':
          {
            const char *saved_arg = arg;
            char *digits_end = NULL;
            while (*arg == 'r')
              arg++;
            base_revision = strtol(arg, &digits_end, 10);
            if ((! SVN_IS_VALID_REVNUM(base_revision))
                || (! digits_end)
                || *digits_end)
              return svn_error_createf(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                       _("Invalid revision number '%s'"),
                                       saved_arg);
          }
          break;
        case 'B':
          branch_id = (arg[0] == 'B') ? apr_pstrdup(pool, arg)
                                      : apr_psprintf(pool, "B%s", arg);
          break;
        case with_revprop_opt:
          SVN_ERR(svn_opt_parse_revprop(&revprops, arg, pool));
          break;
        case 'X':
          extra_args_file = apr_pstrdup(pool, arg);
          break;
        case non_interactive_opt:
          non_interactive = TRUE;
          break;
        case force_interactive_opt:
          force_interactive = TRUE;
          break;
        case trust_server_cert_opt:
          trust_unknown_ca = TRUE;
          break;
        case trust_server_cert_failures_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&opt_arg, arg, pool));
          SVN_ERR(svn_cmdline__parse_trust_options(
                      &trust_unknown_ca,
                      &trust_cn_mismatch,
                      &trust_expired,
                      &trust_not_yet_valid,
                      &trust_other_failure,
                      opt_arg, pool));
          break;
        case config_dir_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&config_dir, arg, pool));
          break;
        case config_inline_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&opt_arg, arg, pool));
          SVN_ERR(svn_cmdline__parse_config_option(config_options, opt_arg,
                                                   "svnmover: ", pool));
          break;
        case no_auth_cache_opt:
          no_auth_cache = TRUE;
          break;
        case version_opt:
          show_version = TRUE;
          break;
        case ui_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&opt_arg, arg, pool));
          SVN_ERR(svn_token__from_word_err(&the_ui_mode, ui_mode_map, opt_arg));
          break;
        case 'h':
        case '?':
          usage(stdout, pool);
          return SVN_NO_ERROR;
        }
    }

  if (show_version)
    {
      SVN_ERR(display_version(opts, quiet, pool));
      return SVN_NO_ERROR;
    }

  if (non_interactive && force_interactive)
    {
      return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                              _("--non-interactive and --force-interactive "
                                "are mutually exclusive"));
    }
  else
    non_interactive = !svn_cmdline__be_interactive(non_interactive,
                                                   force_interactive);

  if (!non_interactive)
    {
      if (trust_unknown_ca || trust_cn_mismatch || trust_expired
          || trust_not_yet_valid || trust_other_failure)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                _("--trust-server-cert-failures requires "
                                  "--non-interactive"));
    }


  /* Now initialize the client context */

  err = svn_config_get_config(&cfg_hash, config_dir, pool);
  if (err)
    {
      /* Fallback to default config if the config directory isn't readable
         or is not a directory. */
      if (APR_STATUS_IS_EACCES(err->apr_err)
          || SVN__APR_STATUS_IS_ENOTDIR(err->apr_err))
        {
          svn_handle_warning2(stderr, err, "svnmover: ");
          svn_error_clear(err);

          SVN_ERR(svn_config__get_default_config(&cfg_hash, pool));
        }
      else
        return err;
    }

  if (config_options)
    {
      svn_error_clear(
          svn_cmdline__apply_config_options(cfg_hash, config_options,
                                            "svnmover: ", "--config-option"));
    }

  SVN_ERR(svn_client_create_context2(&ctx, cfg_hash, pool));

  cfg_config = svn_hash_gets(cfg_hash, SVN_CONFIG_CATEGORY_CONFIG);
  SVN_ERR(svn_cmdline_create_auth_baton2(&ctx->auth_baton,
                                         non_interactive,
                                         username,
                                         password,
                                         config_dir,
                                         no_auth_cache,
                                         trust_unknown_ca,
                                         trust_cn_mismatch,
                                         trust_expired,
                                         trust_not_yet_valid,
                                         trust_other_failure,
                                         cfg_config,
                                         ctx->cancel_func,
                                         ctx->cancel_baton,
                                         pool));

  /* Get the commit log message */
  SVN_ERR(get_log_message(&log_msg, message, revprops, filedata,
                          pool, pool));

  /* Put the log message in the list of revprops, and check that the user
     did not try to supply any other "svn:*" revprops. */
  if (svn_prop_has_svn_prop(revprops, pool))
    return svn_error_create(SVN_ERR_CLIENT_PROPERTY_NAME, NULL,
                            _("Standard properties can't be set "
                              "explicitly as revision properties"));
  if (log_msg)
    {
      svn_hash_sets(revprops, SVN_PROP_REVISION_LOG,
                    svn_string_create(log_msg, pool));
    }

  /* Help command: if given before any actions, then display full help
     (and ANCHOR_URL need not have been provided). */
  if (opts->ind < opts->argc && strcmp(opts->argv[opts->ind], "help") == 0)
    {
      usage(stdout, pool);
      return SVN_NO_ERROR;
    }

  if (!anchor_url)
    return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                             "--root-url (-U) not provided");

  /* Copy the rest of our command-line arguments to an array,
     UTF-8-ing them along the way. */
  /* If there are extra arguments in a supplementary file, tack those
     on, too (again, in UTF8 form). */
  action_args = apr_array_make(pool, opts->argc, sizeof(const char *));
  if (extra_args_file)
    {
      const char *extra_args_file_utf8;
      svn_stringbuf_t *contents, *contents_utf8;

      SVN_ERR(svn_utf_cstring_to_utf8(&extra_args_file_utf8,
                                      extra_args_file, pool));
      SVN_ERR(svn_stringbuf_from_file2(&contents, extra_args_file_utf8, pool));
      SVN_ERR(svn_utf_stringbuf_to_utf8(&contents_utf8, contents, pool));
      svn_cstring_split_append(action_args, contents_utf8->data, "\n\r",
                               FALSE, pool);
    }

  interactive_actions = !(opts->ind < opts->argc
                          || extra_args_file
                          || non_interactive);

  if (interactive_actions)
    {
      linenoiseSetCompletionCallback(linenoise_completion);
    }

  SVN_ERR(wc_create(&wc,
                    anchor_url, base_revision,
                    branch_id,
                    ctx, pool, pool));

  do
    {
      /* Parse arguments -- converting local style to internal style,
       * repos-relative URLs to regular URLs, etc. */
      err = svn_client_args_to_target_array2(&action_args, opts, action_args,
                                             ctx, FALSE, pool);
      if (! err)
        err = parse_actions(&actions, action_args, pool);
      if (! err)
        err = execute(wc, actions, anchor_url, revprops, ctx, pool);
      if (err)
        {
          if (err->apr_err == SVN_ERR_AUTHN_FAILED && non_interactive)
            err = svn_error_quick_wrap(err,
                                       _("Authentication failed and interactive"
                                         " prompting is disabled; see the"
                                         " --force-interactive option"));
          if (interactive_actions)
            {
              /* Display the error, but don't quit */
              svn_handle_error2(err, stderr, FALSE, "svnmover: ");
              svn_error_clear(err);
            }
          else
            SVN_ERR(err);
        }

      /* Possibly read more actions from the command line */
      if (interactive_actions)
        {
          SVN_ERR(read_words(&action_args, "svnmover> ", pool));
        }
    }
  while (interactive_actions && action_args);

  SVN_ERR(final_commit(wc, revprops, pool));

  return SVN_NO_ERROR;
}

int
main(int argc, const char *argv[])
{
  apr_pool_t *pool;
  int exit_code = EXIT_SUCCESS;
  svn_error_t *err;

  /* Initialize the app. */
  if (svn_cmdline_init("svnmover", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  /* Create our top-level pool.  Use a separate mutexless allocator,
   * given this application is single threaded.
   */
  pool = apr_allocator_owner_get(svn_pool_create_allocator(FALSE));

  svn_error_set_malfunction_handler(svn_error_raise_on_malfunction);

  err = sub_main(&exit_code, argc, argv, pool);

  /* Flush stdout and report if it fails. It would be flushed on exit anyway
     but this makes sure that output is not silently lost if it fails. */
  err = svn_error_compose_create(err, svn_cmdline_fflush(stdout));

  if (err)
    {
      exit_code = EXIT_FAILURE;
      svn_cmdline_handle_exit_error(err, NULL, "svnmover: ");
    }

  svn_pool_destroy(pool);
  return exit_code;
}