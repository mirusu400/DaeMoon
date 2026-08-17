/* What a run over a whole library is allowed to do, and what it is called.
 *
 * Separate from batch.c, which draws it, for the same reason welcome_steps.c is
 * separate: the order of the options and which conflict policy each one means are
 * decisions, and a decision only a console can check is one that gets checked by
 * installing a CIA.
 *
 * The order matters more here than on most screens. The list is read top to bottom
 * and the cursor starts at the top, so the least destructive answer goes first: a
 * mispress on this screen is a mispress applied to every title at once.
 */
#include "daemoon_3ds.h"

static const daemoon_str_id_t k_op_labels[] = {
    DAEMOON_STR_BATCH_BACKUP,
    DAEMOON_STR_BATCH_SYNC
};

static const daemoon_str_id_t k_op_hints[] = {
    DAEMOON_STR_BATCH_BACKUP_HINT,
    DAEMOON_STR_BATCH_SYNC_HINT
};

/* Ask first, and it is where the cursor starts.
 *
 * The two policies below it are not dangerous in the "a save is gone" sense -
 * uploading leaves every server version in place, and downloading backs the
 * console's save up to the card on the way past - but they are decisions applied
 * to a whole library without being read, and defaulting to one of them would make
 * the fast path the unread one. */
static const daemoon_str_id_t k_policy_labels[] = {
    DAEMOON_STR_BATCH_POLICY_ASK,
    DAEMOON_STR_BATCH_POLICY_LOCAL,
    DAEMOON_STR_BATCH_POLICY_SERVER
};

static const daemoon_str_id_t k_policy_hints[] = {
    DAEMOON_STR_BATCH_POLICY_ASK_HINT,
    DAEMOON_STR_BATCH_POLICY_LOCAL_HINT,
    DAEMOON_STR_BATCH_POLICY_SERVER_HINT
};

static const daemoon_conflict_policy_t k_policies[] = {
    DAEMOON_CONFLICT_POLICY_ASK,
    DAEMOON_CONFLICT_POLICY_KEEP_LOCAL,
    DAEMOON_CONFLICT_POLICY_KEEP_SERVER
};

size_t daemoon_3ds_batch_ops(void)
{
    return sizeof(k_op_labels) / sizeof(k_op_labels[0]);
}

daemoon_str_id_t daemoon_3ds_batch_op_label(size_t index)
{
    return k_op_labels[index < daemoon_3ds_batch_ops() ? index : 0];
}

daemoon_str_id_t daemoon_3ds_batch_op_hint(size_t index)
{
    return k_op_hints[index < daemoon_3ds_batch_ops() ? index : 0];
}

size_t daemoon_3ds_batch_policies(void)
{
    return sizeof(k_policy_labels) / sizeof(k_policy_labels[0]);
}

daemoon_str_id_t daemoon_3ds_batch_policy_label(size_t index)
{
    return k_policy_labels[index < daemoon_3ds_batch_policies() ? index : 0];
}

daemoon_str_id_t daemoon_3ds_batch_policy_hint(size_t index)
{
    return k_policy_hints[index < daemoon_3ds_batch_policies() ? index : 0];
}

daemoon_conflict_policy_t daemoon_3ds_batch_policy(size_t index)
{
    /* Out of range is Ask, not the last entry.
     *
     * A list index that has run off the end is a bug, and the answer to a bug on
     * this screen has to be the one that still puts a person in front of every
     * decision rather than the one that applies a policy to a library. */
    return k_policies[index < daemoon_3ds_batch_policies() ? index : 0];
}
