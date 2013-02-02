* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "mail-namespace.h"
#include "dsync-mailbox-tree.h"
#include "dsync-ibc.h"
#include "dsync-brain-private.h"

static const char *dsync_state_names[DSYNC_STATE_DONE+1] = {
	"recv_handshake",
	"send_last_common",
	"recv_last_common",
	"send_mailbox_tree",
	"send_mailbox_tree_deletes",
	"recv_mailbox_tree",
	"recv_mailbox_tree_deletes",
	"master_send_mailbox",
	"slave_recv_mailbox",
	"sync_mails",
	"done"
};

static void dsync_brain_run_io(void *context)
{
	struct dsync_brain *brain = context;
	bool changed, try_pending;

	if (dsync_ibc_has_failed(brain->ibc)) {
		io_loop_stop(current_ioloop);
		brain->failed = TRUE;
		return;
	}

	try_pending = TRUE;
	do {
		if (!dsync_brain_run(brain, &changed)) {
			io_loop_stop(current_ioloop);
			break;
		}
		if (changed)
			try_pending = TRUE;
		else if (try_pending) {
			if (dsync_ibc_has_pending_data(brain->ibc))
				changed = TRUE;
			try_pending = FALSE;
		}
	} while (changed);
}

static struct dsync_brain *
dsync_brain_common_init(struct mail_user *user, struct dsync_ibc *ibc)
{
	struct dsync_brain *brain;
	pool_t pool;

	pool = pool_alloconly_create("dsync brain", 10240);
	brain = p_new(pool, struct dsync_brain, 1);
	brain->pool = pool;
	brain->user = user;
	brain->ibc = ibc;
	brain->sync_type = DSYNC_BRAIN_SYNC_TYPE_UNKNOWN;
	hash_table_create(&brain->mailbox_states, pool, 0,
			  guid_128_hash, guid_128_cmp);
	p_array_init(&brain->remote_mailbox_states, pool, 64);
	return brain;
}

static void
dsync_brain_set_flags(struct dsync_brain *brain, enum dsync_brain_flags flags)
{
	brain->guid_requests =
		(flags & DSYNC_BRAIN_FLAG_SEND_GUID_REQUESTS) != 0;
	brain->mails_have_guids =
		(flags & DSYNC_BRAIN_FLAG_MAILS_HAVE_GUIDS) != 0;
	brain->backup_send = (flags & DSYNC_BRAIN_FLAG_BACKUP_SEND) != 0;
	brain->backup_recv = (flags & DSYNC_BRAIN_FLAG_BACKUP_RECV) != 0;
	brain->debug = (flags & DSYNC_BRAIN_FLAG_DEBUG) != 0;
	brain->sync_all_namespaces =
		(flags & DSYNC_BRAIN_FLAG_SYNC_ALL_NAMESPACES) != 0;
}

struct dsync_brain *
dsync_brain_master_init(struct mail_user *user, struct dsync_ibc *ibc,
			struct mail_namespace *sync_ns, const char *sync_box,
			enum dsync_brain_sync_type sync_type,
			enum dsync_brain_flags flags,
			const char *state)
{
	struct dsync_ibc_settings ibc_set;
	struct dsync_brain *brain;
	const char *error;

	i_assert(sync_type != DSYNC_BRAIN_SYNC_TYPE_UNKNOWN);
	i_assert(sync_type != DSYNC_BRAIN_SYNC_TYPE_STATE || *state != '\0');

	brain = dsync_brain_common_init(user, ibc);
	brain->sync_type = sync_type;
	if (sync_ns != NULL)
		brain->sync_ns = sync_ns;
	brain->sync_box = p_strdup(brain->pool, sync_box);
	brain->master_brain = TRUE;
	dsync_brain_set_flags(brain, flags);

	brain->state = DSYNC_STATE_SEND_MAILBOX_TREE;
	if (sync_type == DSYNC_BRAIN_SYNC_TYPE_STATE) {
		if (dsync_mailbox_states_import(brain->mailbox_states,
						brain->pool, state,
						&error) < 0) {
			hash_table_clear(brain->mailbox_states, FALSE);
			i_error("Saved sync state is invalid, "
				"falling back to full sync: %s", error);
			brain->sync_type = sync_type =
				DSYNC_BRAIN_SYNC_TYPE_FULL;
		} else {
			brain->state = DSYNC_STATE_MASTER_SEND_LAST_COMMON;
		}
	}
	dsync_brain_mailbox_trees_init(brain);

	memset(&ibc_set, 0, sizeof(ibc_set));
	ibc_set.sync_ns_prefix = sync_ns == NULL ? NULL : sync_ns->prefix;
	ibc_set.sync_box = sync_box;
	ibc_set.sync_type = sync_type;
	/* reverse the backup direction for the slave */
	ibc_set.brain_flags = flags & ~(DSYNC_BRAIN_FLAG_BACKUP_SEND |
					DSYNC_BRAIN_FLAG_BACKUP_RECV);
	if ((flags & DSYNC_BRAIN_FLAG_BACKUP_SEND) != 0)
		ibc_set.brain_flags |= DSYNC_BRAIN_FLAG_BACKUP_RECV;
	else if ((flags & DSYNC_BRAIN_FLAG_BACKUP_RECV) != 0)
		ibc_set.brain_flags |= DSYNC_BRAIN_FLAG_BACKUP_SEND;
	dsync_ibc_send_handshake(ibc, &ibc_set);

	dsync_ibc_set_io_callback(ibc, dsync_brain_run_io, brain);
	return brain;
}

struct dsync_brain *
dsync_brain_slave_init(struct mail_user *user, struct dsync_ibc *ibc)
{
	struct dsync_brain *brain;

	brain = dsync_brain_common_init(user, ibc);
	brain->state = DSYNC_STATE_SLAVE_RECV_HANDSHAKE;

	dsync_ibc_set_io_callback(ibc, dsync_brain_run_io, brain);
	return brain;
}

int dsync_brain_deinit(struct dsync_brain **_brain)
{
	struct dsync_brain *brain = *_brain;
	int ret;

	*_brain = NULL;

	if (dsync_ibc_has_failed(brain->ibc) ||
	    brain->state != DSYNC_STATE_DONE)
		brain->failed = TRUE;
	dsync_ibc_close_mail_streams(brain->ibc);

	if (brain->box != NULL)
		dsync_brain_sync_mailbox_deinit(brain);
	if (brain->local_tree_iter != NULL)
		dsync_mailbox_tree_iter_deinit(&brain->local_tree_iter);
	if (brain->mailbox_states_iter != NULL)
		hash_table_iterate_deinit(&brain->mailbox_states_iter);
	hash_table_destroy(&brain->mailbox_states);

	ret = brain->failed ? -1 : 0;
	pool_unref(&brain->pool);
	return ret;
}

static bool dsync_brain_slave_recv_handshake(struct dsync_brain *brain)
{
	const struct dsync_ibc_settings *ibc_set;

	i_assert(!brain->master_brain);

	if (dsync_ibc_recv_handshake(brain->ibc, &ibc_set) == 0)
		return FALSE;

	if (ibc_set->sync_ns_prefix != NULL) {
		brain->sync_ns = mail_namespace_find(brain->user->namespaces,
						     ibc_set->sync_ns_prefix);
	}
	brain->sync_box = p_strdup(brain->pool, ibc_set->sync_box);
	i_assert(brain->sync_type == DSYNC_BRAIN_SYNC_TYPE_UNKNOWN);
	brain->sync_type = ibc_set->sync_type;
	dsync_brain_set_flags(brain, ibc_set->brain_flags);

	dsync_brain_mailbox_trees_init(brain);

	if (brain->sync_type == DSYNC_BRAIN_SYNC_TYPE_STATE)
		brain->state = DSYNC_STATE_SLAVE_RECV_LAST_COMMON;
	else
		brain->state = DSYNC_STATE_SEND_MAILBOX_TREE;
	return TRUE;
}

static void dsync_brain_master_send_last_common(struct dsync_brain *brain)
{
	struct dsync_mailbox_state *state;
	uint8_t *guid;
	enum dsync_ibc_send_ret ret = DSYNC_IBC_SEND_RET_OK;

	i_assert(brain->master_brain);

	if (brain->mailbox_states_iter == NULL) {
		brain->mailbox_states_iter =
			hash_table_iterate_init(brain->mailbox_states);
	}

	for (;;) {
		if (ret == DSYNC_IBC_SEND_RET_FULL)
			return;
		if (!hash_table_iterate(brain->mailbox_states_iter,
					brain->mailbox_states, &guid, &state))
			break;
		ret = dsync_ibc_send_mailbox_state(brain->ibc, state);
	}
	hash_table_iterate_deinit(&brain->mailbox_states_iter);

	dsync_ibc_send_end_of_list(brain->ibc);
	brain->state = DSYNC_STATE_SEND_MAILBOX_TREE;
}

static void dsync_mailbox_state_add(struct dsync_brain *brain,
				    const struct dsync_mailbox_state *state)
{
	struct dsync_mailbox_state *dupstate;
	uint8_t *guid_p;

	dupstate = p_new(brain->pool, struct dsync_mailbox_state, 1);
	*dupstate = *state;
	guid_p = dupstate->mailbox_guid;
	hash_table_insert(brain->mailbox_states, guid_p, dupstate);
}

static bool dsync_brain_slave_recv_last_common(struct dsync_brain *brain)
{
	struct dsync_mailbox_state state;
	enum dsync_ibc_recv_ret ret;
	bool changed = FALSE;

	i_assert(!brain->master_brain);

	while ((ret = dsync_ibc_recv_mailbox_state(brain->ibc, &state)) > 0) {
		dsync_mailbox_state_add(brain, &state);
		changed = TRUE;
	}
	if (ret == DSYNC_IBC_RECV_RET_FINISHED) {
		brain->state = DSYNC_STATE_SEND_MAILBOX_TREE;
		changed = TRUE;
	}
	return changed;
}

static bool dsync_brain_run_real(struct dsync_brain *brain, bool *changed_r)
{
	bool changed = FALSE, ret = TRUE;

	if (brain->failed)
		return FALSE;

	if (brain->debug) {
		i_debug("brain %c: in state=%s", brain->master_brain ? 'M' : 'S',
			dsync_state_names[brain->state]);
	}
	switch (brain->state) {
	case DSYNC_STATE_SLAVE_RECV_HANDSHAKE:
		changed = dsync_brain_slave_recv_handshake(brain);
		break;
	case DSYNC_STATE_MASTER_SEND_LAST_COMMON:
		dsync_brain_master_send_last_common(brain);
		changed = TRUE;
		break;
	case DSYNC_STATE_SLAVE_RECV_LAST_COMMON:
		changed = dsync_brain_slave_recv_last_common(brain);
		break;
	case DSYNC_STATE_SEND_MAILBOX_TREE:
		dsync_brain_send_mailbox_tree(brain);
		changed = TRUE;
		break;
	case DSYNC_STATE_RECV_MAILBOX_TREE:
		changed = dsync_brain_recv_mailbox_tree(brain);
		break;
	case DSYNC_STATE_SEND_MAILBOX_TREE_DELETES:
		dsync_brain_send_mailbox_tree_deletes(brain);
		changed = TRUE;
		break;
	case DSYNC_STATE_RECV_MAILBOX_TREE_DELETES:
		changed = dsync_brain_recv_mailbox_tree_deletes(brain);
		break;
	case DSYNC_STATE_MASTER_SEND_MAILBOX:
		dsync_brain_master_send_mailbox(brain);
		changed = TRUE;
		break;
	case DSYNC_STATE_SLAVE_RECV_MAILBOX:
		changed = dsync_brain_slave_recv_mailbox(brain);
		break;
	case DSYNC_STATE_SYNC_MAILS:
		changed = dsync_brain_sync_mails(brain);
		break;
	case DSYNC_STATE_DONE:
		changed = TRUE;
		ret = FALSE;
		break;
	}
	if (brain->debug) {
		i_debug("brain %c: out state=%s changed=%d",
			brain->master_brain ? 'M' : 'S',
			dsync_state_names[brain->state], changed);
	}

	*changed_r = changed;
	return brain->failed ? FALSE : ret;
}

bool dsync_brain_run(struct dsync_brain *brain, bool *changed_r)
{
	bool ret;

	*changed_r = FALSE;

	if (dsync_ibc_has_failed(brain->ibc)) {
		brain->failed = TRUE;
		return FALSE;
	}

	T_BEGIN {
		ret = dsync_brain_run_real(brain, changed_r);
	} T_END;
	return ret;
}

void dsync_brain_get_state(struct dsync_brain *brain, string_t *output)
{
	struct hash_iterate_context *iter;
	struct dsync_mailbox_node *node;
	const struct dsync_mailbox_state *new_state;
	struct dsync_mailbox_state *state;
	const uint8_t *guid_p;
	uint8_t *guid;

	/* update mailbox states */
	array_foreach(&brain->remote_mailbox_states, new_state) {
		guid_p = new_state->mailbox_guid;
		state = hash_table_lookup(brain->mailbox_states, guid_p);
		if (state != NULL)
			*state = *new_state;
		else
			dsync_mailbox_state_add(brain, new_state);
	}

	/* remove nonexistent mailboxes */
	iter = hash_table_iterate_init(brain->mailbox_states);
	while (hash_table_iterate(iter, brain->mailbox_states, &guid, &state)) {
		node = dsync_mailbox_tree_lookup_guid(brain->local_mailbox_tree,
						      guid);
		if (node == NULL ||
		    node->existence != DSYNC_MAILBOX_NODE_EXISTS)
			hash_table_remove(brain->mailbox_states, guid);
	}
	hash_table_iterate_deinit(&iter);

	dsync_mailbox_states_export(brain->mailbox_states, output);
}

bool dsync_brain_has_failed(struct dsync_brain *brain)
{
	return brain->failed;
}
