
#include "picocoin-config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <openssl/bn.h>
#include <glib.h>
#include "blkdb.h"
#include "message.h"
#include "serialize.h"

static bool fread_message(int fd, struct p2p_message *msg, bool *read_ok)
{
	*read_ok = false;

	if (msg->data) {
		free(msg->data);
		msg->data = NULL;
	}

	unsigned char hdrbuf[P2P_HDR_SZ];

	ssize_t rrc = read(fd, hdrbuf, sizeof(hdrbuf));
	if (rrc != sizeof(hdrbuf)) {
		if (rrc == 0)
			*read_ok = true;
		return false;
	}

	parse_message_hdr(&msg->hdr, hdrbuf);

	unsigned int data_len = msg->hdr.data_len;
	if (data_len > (100 * 1024 * 1024))
		return false;
	
	msg->data = malloc(data_len);

	rrc = read(fd, msg->data, data_len);
	if (rrc != data_len)
		goto err_out_data;

	if (!message_valid(msg))
		goto err_out_data;

	*read_ok = true;
	return true;

err_out_data:
	free(msg->data);
	msg->data = NULL;

	return false;
}

static guint blk_hash(gconstpointer key_)
{
	const guint *key = key_;

	return key[4];	/* return a random int in the middle of the 32b hash */
}

static gboolean blk_equal(gconstpointer a, gconstpointer b)
{
	const unsigned char *hash1 = a;
	const unsigned char *hash2 = b;

	return memcmp(hash1, hash2, 32) == 0;
}

bool blkdb_init(struct blkdb *db, const unsigned char *netmagic,
		const char *genesis_hash)
{
	memset(db, 0, sizeof(*db));

	db->fd = -1;

	BN_hex2bn(&db->block0, genesis_hash);

	memcpy(db->netmagic, netmagic, sizeof(db->netmagic));
	db->blocks = g_hash_table_new_full(blk_hash, blk_equal, NULL, g_free);

	return true;
}

static bool blkdb_read_rec(struct blkdb *db, const struct p2p_message *msg)
{
	struct blkinfo *bi;
	struct buffer buf = { msg->data, msg->hdr.data_len };

	if (strncmp(msg->hdr.command, "rec", 12))
		return false;

	bi = calloc(1, sizeof(*bi));
	BIGNUM blkhash;
	BN_init(&blkhash);

	/* deserialize record */
	if (!deser_u256(&blkhash, &buf))
		goto err_out;
	if (!deser_bp_block(&bi->hdr, &buf))
		goto err_out;
	
	/* verify that provided hash matches block header, as an additional
	 * self-verification step
	 */
	bp_block_calc_sha256(&bi->hdr);
	if (BN_cmp(&blkhash, &bi->hdr.sha256) != 0)
		goto err_out;

	/* copy serialized block hash, to be used as hash table index */
	memcpy(&bi->ser_hash[0], msg->data, sizeof(bi->ser_hash));

	/* verify genesis block matches first record */
	if ((g_hash_table_size(db->blocks) == 0) &&
	    (BN_cmp(&blkhash, db->block0) != 0))
		goto err_out;

	/* add to block map */
	g_hash_table_insert(db->blocks, bi->ser_hash, bi);

	BN_clear_free(&blkhash);
	return true;

err_out:
	free(bi);
	BN_clear_free(&blkhash);
	return false;
}

static GString *ser_blkinfo(const struct blkinfo *bi)
{
	GString *rs = g_string_sized_new(sizeof(*bi));

	g_string_append_len(rs, (gchar *) bi->ser_hash, sizeof(bi->ser_hash));
	ser_bp_block(rs, &bi->hdr);

	return rs;
}

static GString *blkdb_ser_rec(struct blkdb *db, const struct blkinfo *bi)
{
	GString *data = ser_blkinfo(bi);

	GString *rs = message_str(db->netmagic, "rec", data->str, data->len);

	g_string_free(data, TRUE);

	return rs;
}

bool blkdb_read(struct blkdb *db, const char *idx_fn)
{
	bool rc = true;
	int fd = open(idx_fn, O_RDONLY);
	if (fd < 0)
		return false;

	struct p2p_message msg;
	memset(&msg, 0, sizeof(msg));
	bool read_ok = true;

	while (fread_message(fd, &msg, &read_ok)) {
		rc = blkdb_read_rec(db, &msg);
		if (!rc)
			break;
	}

	close(fd);

	free(msg.data);

	return read_ok && rc;
}

bool blkdb_add(struct blkdb *db, struct blkinfo *bi)
{
	if (db->fd >= 0) {
		GString *data = blkdb_ser_rec(db, bi);
		if (!data)
			return false;

		/* assume either at EOF, or O_APPEND */
		size_t data_len = data->len;
		ssize_t wrc = write(db->fd, data->str, data_len);

		g_string_free(data, TRUE);

		if (wrc != data_len)
			return false;

		if (db->datasync_fd && (fdatasync(db->fd) < 0))
			return false;
	}

	/* add to block map */
	g_hash_table_insert(db->blocks, bi->ser_hash, bi);

	return true;
}

void blkdb_free(struct blkdb *db)
{
	if (db->close_fd && (db->fd >= 0))
		close(db->fd);

	BN_clear_free(db->block0);

	g_hash_table_unref(db->blocks);
}

