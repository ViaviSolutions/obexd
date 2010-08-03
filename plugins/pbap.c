/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2009-2010  Intel Corporation
 *  Copyright (C) 2007-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <openobex/obex.h>
#include <openobex/obex_const.h>

#include "plugin.h"
#include "log.h"
#include "obex.h"
#include "service.h"
#include "phonebook.h"
#include "mimetype.h"
#include "filesystem.h"
#include "dbus.h"

#define PHONEBOOK_TYPE		"x-bt/phonebook"
#define VCARDLISTING_TYPE	"x-bt/vcard-listing"
#define VCARDENTRY_TYPE		"x-bt/vcard"

#define ORDER_TAG		0x01
#define SEARCHVALUE_TAG		0x02
#define SEARCHATTRIB_TAG	0x03
#define MAXLISTCOUNT_TAG	0x04
#define LISTSTARTOFFSET_TAG	0x05
#define FILTER_TAG		0x06
#define FORMAT_TAG		0X07
#define PHONEBOOKSIZE_TAG	0X08
#define NEWMISSEDCALLS_TAG	0X09

/* The following length is in the unit of byte */
#define ORDER_LEN		1
#define SEARCHATTRIB_LEN	1
#define MAXLISTCOUNT_LEN	2
#define LISTSTARTOFFSET_LEN	2
#define FILTER_LEN		8
#define FORMAT_LEN		1
#define PHONEBOOKSIZE_LEN	2
#define NEWMISSEDCALLS_LEN	1

#define PBAP_CHANNEL	15

#define PBAP_RECORD "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>	\
<record>								\
  <attribute id=\"0x0001\">						\
    <sequence>								\
      <uuid value=\"0x112f\"/>						\
    </sequence>								\
  </attribute>								\
									\
  <attribute id=\"0x0004\">						\
    <sequence>								\
      <sequence>							\
        <uuid value=\"0x0100\"/>					\
      </sequence>							\
      <sequence>							\
        <uuid value=\"0x0003\"/>					\
        <uint8 value=\"%u\" name=\"channel\"/>				\
      </sequence>							\
      <sequence>							\
        <uuid value=\"0x0008\"/>					\
      </sequence>							\
    </sequence>								\
  </attribute>								\
									\
  <attribute id=\"0x0009\">						\
    <sequence>								\
      <sequence>							\
        <uuid value=\"0x1130\"/>					\
        <uint16 value=\"0x0100\" name=\"version\"/>			\
      </sequence>							\
    </sequence>								\
  </attribute>								\
									\
  <attribute id=\"0x0100\">						\
    <text value=\"%s\" name=\"name\"/>					\
  </attribute>								\
									\
  <attribute id=\"0x0314\">						\
    <uint8 value=\"0x01\"/>						\
  </attribute>								\
</record>"

struct aparam_header {
	uint8_t tag;
	uint8_t len;
	uint8_t val[0];
} __attribute__ ((packed));

struct cache {
	gboolean valid;
	uint32_t index;
	char *folder;
	GSList *entries;
};

struct cache_entry {
	uint32_t handle;
	char *id;
	char *name;
	char *sound;
	char *tel;
};

struct pbap_session {
	struct apparam_field *params;
	char *folder;
	uint32_t find_handle;
	GString *buffer;
	struct cache cache;
};

static const uint8_t PBAP_TARGET[TARGET_SIZE] = {
			0x79, 0x61, 0x35, 0xF0,  0xF0, 0xC5, 0x11, 0xD8,
			0x09, 0x66, 0x08, 0x00,  0x20, 0x0C, 0x9A, 0x66  };

typedef int (*cache_entry_find_f) (const struct cache_entry *entry,
			const char *value);

static void cache_entry_free(struct cache_entry *entry)
{
	g_free(entry->id);
	g_free(entry->name);
	g_free(entry->sound);
	g_free(entry->tel);
	g_free(entry);
}

static gboolean entry_name_find(const struct cache_entry *entry,
		const char *value)
{
	char *name;
	gboolean ret;

	if (!entry->name)
		return FALSE;

	if (strlen(value) == 0)
		return TRUE;

	name = g_utf8_strdown(entry->name, -1);
	ret = (g_strstr_len(name, -1, value) ? TRUE : FALSE);
	g_free(name);

	return ret;
}

static gboolean entry_sound_find(const struct cache_entry *entry,
		const char *value)
{
	if (!entry->sound)
		return FALSE;

	return (g_strstr_len(entry->sound, -1, value) ? TRUE : FALSE);
}

static gboolean entry_tel_find(const struct cache_entry *entry,
		const char *value)
{
	if (!entry->tel)
		return FALSE;

	return (g_strstr_len(entry->tel, -1, value) ? TRUE : FALSE);
}

static const char *cache_find(struct cache *cache, uint32_t handle)
{
	GSList *l;

	for (l = cache->entries; l; l = l->next) {
		struct cache_entry *entry = l->data;

		if (entry->handle == handle)
			return entry->id;
	}

	return NULL;
}

static void cache_clear(struct cache *cache)
{
	g_free(cache->folder);
	g_slist_foreach(cache->entries, (GFunc) cache_entry_free, NULL);
	g_slist_free(cache->entries);
	cache->entries = NULL;
}

static void phonebook_size_result(const char *buffer, size_t bufsize,
				int vcards, int missed, void *user_data)
{
	struct pbap_session *pbap = user_data;
	char aparam[4];
	struct aparam_header *hdr = (struct aparam_header *) aparam;
	uint16_t phonebooksize;

	DBG("vcards %d", vcards);

	phonebooksize = htons(vcards);

	hdr->tag = PHONEBOOKSIZE_TAG;
	hdr->len = PHONEBOOKSIZE_LEN;
	memcpy(hdr->val, &phonebooksize, sizeof(phonebooksize));

	pbap->buffer = g_string_new_len(aparam, sizeof(aparam));

	obex_object_set_io_flags(pbap, G_IO_IN, 0);
}

static void query_result(const char *buffer, size_t bufsize, int vcards,
					int missed, void *user_data)
{
	struct pbap_session *pbap = user_data;

	DBG("");

	if (!pbap->buffer)
		pbap->buffer = g_string_new_len(buffer, bufsize);
	else
		pbap->buffer = g_string_append_len(pbap->buffer, buffer,
								bufsize);

	obex_object_set_io_flags(pbap, G_IO_IN, 0);
}

static void cache_entry_notify(const char *id, uint32_t handle,
					const char *name, const char *sound,
					const char *tel, void *user_data)
{
	struct pbap_session *pbap = user_data;
	struct cache_entry *entry = g_new0(struct cache_entry, 1);
	struct cache *cache = &pbap->cache;

	if (handle != PHONEBOOK_INVALID_HANDLE)
		entry->handle = handle;
	else
		entry->handle = ++pbap->cache.index;

	entry->id = g_strdup(id);
	entry->name = g_strdup(name);
	entry->sound = g_strdup(sound);
	entry->tel = g_strdup(tel);

	cache->entries = g_slist_append(cache->entries, entry);
}

static int alpha_sort(gconstpointer a, gconstpointer b)
{
	const struct cache_entry *e1 = a;
	const struct cache_entry *e2 = b;

	return g_strcmp0(e1->name, e2->name);
}

static int indexed_sort(gconstpointer a, gconstpointer b)
{
	const struct cache_entry *e1 = a;
	const struct cache_entry *e2 = b;

	return (e1->handle - e2->handle);
}

static int phonetical_sort(gconstpointer a, gconstpointer b)
{
	const struct cache_entry *e1 = a;
	const struct cache_entry *e2 = b;

	/* SOUND attribute is optional. Use Indexed sort if not present. */
	if (!e1->sound || !e2->sound)
		return indexed_sort(a, b);

	return g_strcmp0(e1->sound, e2->sound);
}

static GSList *sort_entries(GSList *l, uint8_t order, uint8_t search_attrib,
							const char *value)
{
	GSList *sorted = NULL;
	cache_entry_find_f find;
	GCompareFunc sort;
	char *searchval;

	/*
	 * Default sorter is "Indexed". Some backends doesn't inform the index,
	 * for this case a sequential internal index is assigned.
	 * 0x00 = indexed
	 * 0x01 = alphanumeric
	 * 0x02 = phonetic
	 */
	switch (order) {
	case 0x01:
		sort = alpha_sort;
		break;
	case 0x02:
		sort = phonetical_sort;
		break;
	default:
		sort = indexed_sort;
		break;
	}

	/*
	 * This implementation checks if the given field CONTAINS the
	 * search value(case insensitive). Name is the default field
	 * when the attribute is not provided.
	 */
	switch (search_attrib) {
		/* Number */
		case 1:
			find = entry_tel_find;
			break;
		/* Sound */
		case 2:
			find = entry_sound_find;
			break;
		default:
			find = entry_name_find;
			break;
	}

	searchval = value ? g_utf8_strdown(value, -1) : NULL;
	for (; l; l = l->next) {
		struct cache_entry *entry = l->data;

		if (searchval && !find(entry, (const char *) searchval))
			continue;

		sorted = g_slist_insert_sorted(sorted, entry, sort);
	}

	g_free(searchval);

	return sorted;
}

static void cache_ready_notify(void *user_data)
{
	struct pbap_session *pbap = user_data;
	GSList *sorted;
	GSList *l;
	uint16_t max = pbap->params->maxlistcount;

	DBG("");

	if (max == 0) {
		/* Ignore all other parameter and return PhoneBookSize */
		char aparam[4];
		struct aparam_header *hdr = (struct aparam_header *) aparam;
		uint16_t size = htons(g_slist_length(pbap->cache.entries));

		hdr->tag = PHONEBOOKSIZE_TAG;
		hdr->len = PHONEBOOKSIZE_LEN;
		memcpy(hdr->val, &size, sizeof(size));

		pbap->buffer = g_string_new_len(aparam, sizeof(aparam));
		goto done;
	}
	/*
	 * Don't free the sorted list content: this list contains
	 * only the reference for the "real" cache entry.
	 */
	sorted = sort_entries(pbap->cache.entries, pbap->params->order,
			pbap->params->searchattrib,
			(const char *) pbap->params->searchval);

	/* Computing offset considering first entry of the phonebook */
	l = g_slist_nth(sorted, pbap->params->liststartoffset);

	pbap->buffer = g_string_new(VCARD_LISTING_BEGIN);
	for (; l && max; l = l->next, max--) {
		const struct cache_entry *entry = l->data;

		g_string_append_printf(pbap->buffer, VCARD_LISTING_ELEMENT,
						entry->handle, entry->name);
	}

	pbap->buffer = g_string_append(pbap->buffer, VCARD_LISTING_END);

	g_slist_free(sorted);

done:
	if (!pbap->cache.valid) {
		pbap->cache.valid = TRUE;
		obex_object_set_io_flags(pbap, G_IO_IN, 0);
	}
}

static void cache_entry_done(void *user_data)
{
	struct pbap_session *pbap = user_data;
	const char *id;
	int ret;

	DBG("");

	pbap->cache.valid = TRUE;

	id = cache_find(&pbap->cache, pbap->find_handle);
	if (id == NULL) {
		DBG("Entry %d not found on cache", pbap->find_handle);
		obex_object_set_io_flags(pbap, G_IO_ERR, -ENOENT);
		return;
	}

	ret = phonebook_get_entry(pbap->folder, id, pbap->params,
						query_result, pbap);
	if (ret < 0)
		obex_object_set_io_flags(pbap, G_IO_ERR, ret);
}

static struct apparam_field *parse_aparam(const uint8_t *buffer, uint32_t hlen)
{
	struct apparam_field *param;
	struct aparam_header *hdr;
	uint64_t val64;
	uint32_t len = 0;
	uint16_t val16;

	param = g_new0(struct apparam_field, 1);

	while (len < hlen) {
		hdr = (void *) buffer + len;

		switch (hdr->tag) {
		case ORDER_TAG:
			if (hdr->len != ORDER_LEN)
				goto failed;

			param->order = hdr->val[0];
			break;

		case SEARCHATTRIB_TAG:
			if (hdr->len != SEARCHATTRIB_LEN)
				goto failed;

			param->searchattrib = hdr->val[0];
			break;
		case SEARCHVALUE_TAG:
			if (hdr->len == 0)
				goto failed;

			param->searchval = g_try_malloc0(hdr->len + 1);
			if (param->searchval)
				memcpy(param->searchval, hdr->val, hdr->len);
			break;
		case FILTER_TAG:
			if (hdr->len != FILTER_LEN)
				goto failed;

			memcpy(&val64, hdr->val, sizeof(val64));
			param->filter = GUINT64_FROM_BE(val64);

			break;
		case FORMAT_TAG:
			if (hdr->len != FORMAT_LEN)
				goto failed;

			param->format = hdr->val[0];
			break;
		case MAXLISTCOUNT_TAG:
			if (hdr->len != MAXLISTCOUNT_LEN)
				goto failed;

			memcpy(&val16, hdr->val, sizeof(val16));
			param->maxlistcount = GUINT16_FROM_BE(val16);
			break;
		case LISTSTARTOFFSET_TAG:
			if (hdr->len != LISTSTARTOFFSET_LEN)
				goto failed;

			memcpy(&val16, hdr->val, sizeof(val16));
			param->liststartoffset = GUINT16_FROM_BE(val16);
			break;
		default:
			goto failed;
		}

		len += hdr->len + sizeof(struct aparam_header);
	}

	DBG("o %x sa %x sv %s fil %" G_GINT64_MODIFIER "x for %x max %x off %x",
			param->order, param->searchattrib, param->searchval,
			param->filter, param->format, param->maxlistcount,
			param->liststartoffset);

	return param;

failed:
	g_free(param);

	return NULL;
}

static void *pbap_connect(struct obex_session *os, int *err)
{
	struct pbap_session *pbap;

	manager_register_session(os);

	pbap = g_new0(struct pbap_session, 1);
	pbap->folder = g_strdup("/");
	pbap->find_handle = PHONEBOOK_INVALID_HANDLE;

	if (err)
		*err = 0;

	return pbap;
}

static int pbap_get(struct obex_session *os, obex_object_t *obj,
					gboolean *stream, void *user_data)
{
	struct pbap_session *pbap = user_data;
	const char *type = obex_get_type(os);
	const char *name = obex_get_name(os);
	struct apparam_field *params;
	const uint8_t *buffer;
	char *path;
	ssize_t rsize;
	int ret;

	DBG("name %s type %s pbap %p", name, type, pbap);

	if (type == NULL)
		return -EBADR;

	rsize = obex_aparam_read(os, obj, &buffer);
	if (rsize < 0)
		return -EBADR;

	params = parse_aparam(buffer, rsize);
	if (params == NULL)
		return -EBADR;

	if (pbap->params) {
		g_free(pbap->params->searchval);
		g_free(pbap->params);
	}

	pbap->params = params;

	if (strcmp(type, PHONEBOOK_TYPE) == 0) {
		/* Always contains the absolute path */
		path = g_strdup(name);
		*stream = (params->maxlistcount == 0 ? FALSE : TRUE);
	} else if (strcmp(type, VCARDLISTING_TYPE) == 0) {
		/* Always relative */
		if (!name || strlen(name) == 0)
			/* Current folder */
			path = g_strdup(pbap->folder);
		else
			/* Current folder + relative path */
			path = g_build_filename(pbap->folder, name, NULL);

		*stream = (params->maxlistcount == 0 ? FALSE : TRUE);
	} else if (strcmp(type, VCARDENTRY_TYPE) == 0) {
		/* File name only */
		path = g_strdup(name);
		*stream = TRUE;
	} else
		return -EBADR;

	if (path == NULL)
		return -EBADR;

	pbap->params = params;
	ret = obex_get_stream_start(os, path);

	g_free(path);

	return ret;
}

static int pbap_setpath(struct obex_session *os, obex_object_t *obj,
		void *user_data)
{
	struct pbap_session *pbap = user_data;
	const char *name;
	uint8_t *nonhdr;
	char *fullname;
	int err;

	if (OBEX_ObjectGetNonHdrData(obj, &nonhdr) != 2) {
		error("Set path failed: flag and constants not found!");
		return -EBADMSG;
	}

	name = obex_get_name(os);

	DBG("name %s folder %s nonhdr 0x%x%x", name, pbap->folder,
							nonhdr[0], nonhdr[1]);

	fullname = phonebook_set_folder(pbap->folder, name, nonhdr[0], &err);
	if (err < 0)
		return err;

	g_free(pbap->folder);
	pbap->folder = fullname;

	/*
	 * FIXME: Define a criteria to mark the cache as invalid
	 */
	pbap->cache.valid = FALSE;
	pbap->cache.index = 0;
	cache_clear(&pbap->cache);

	return 0;
}

static void pbap_disconnect(struct obex_session *os, void *user_data)
{
	struct pbap_session *pbap = user_data;

	manager_unregister_session(os);

	if (pbap->params) {
		g_free(pbap->params->searchval);
		g_free(pbap->params);
	}

	cache_clear(&pbap->cache);
	g_free(pbap->folder);
	g_free(pbap);
}

static int pbap_chkput(struct obex_session *os, void *user_data)
{
	/* Rejects all PUTs */
	return -EBADR;
}

static struct obex_service_driver pbap = {
	.name = "Phonebook Access server",
	.service = OBEX_PBAP,
	.channel = PBAP_CHANNEL,
	.record = PBAP_RECORD,
	.target = PBAP_TARGET,
	.target_size = TARGET_SIZE,
	.connect = pbap_connect,
	.get = pbap_get,
	.setpath = pbap_setpath,
	.disconnect = pbap_disconnect,
	.chkput = pbap_chkput
};

static void *vobject_pull_open(const char *name, int oflag, mode_t mode,
				void *context, size_t *size, int *err)
{
	struct pbap_session *pbap = context;
	phonebook_cb cb;
	int ret;

	DBG("name %s context %p maxlistcount %d", name, context,
						pbap->params->maxlistcount);

	if (oflag != O_RDONLY) {
		ret = -EPERM;
		goto fail;
	}

	if (name == NULL) {
		ret = -EBADR;
		goto fail;
	}

	if (pbap->params->maxlistcount == 0)
		cb = phonebook_size_result;
	else
		cb = query_result;

	ret = phonebook_pull(name, pbap->params, cb, pbap);
	if (ret < 0)
		goto fail;

	return pbap;

fail:
	if (err)
		*err = ret;

	return NULL;
}

static void *vobject_list_open(const char *name, int oflag, mode_t mode,
				void *context, size_t *size, int *err)
{
	struct pbap_session *pbap = context;
	int ret;

	DBG("name %s context %p valid %d", name, context, pbap->cache.valid);

	if (oflag != O_RDONLY) {
		ret = -EPERM;
		goto fail;
	}

	if (name == NULL) {
		ret = -EBADR;
		goto fail;
	}

	/* PullvCardListing always get the contacts from the cache */

	if (pbap->cache.valid) {
		cache_ready_notify(pbap);
		goto done;
	}

	ret = phonebook_create_cache(name,
		cache_entry_notify, cache_ready_notify, pbap);

	if (ret < 0)
		goto fail;

done:
	return pbap;

fail:
	if (err)
		*err = ret;

	return NULL;
}

static void *vobject_vcard_open(const char *name, int oflag, mode_t mode,
					void *context, size_t *size, int *err)
{
	struct pbap_session *pbap = context;
	const char *id;
	uint32_t handle;
	int ret;

	DBG("name %s context %p valid %d", name, context, pbap->cache.valid);

	if (oflag != O_RDONLY) {
		ret = -EPERM;
		goto fail;
	}

	if (name == NULL || sscanf(name, "%u.vcf", &handle) != 1) {
		ret = -EBADR;
		goto fail;
	}

	if (pbap->cache.valid == FALSE) {
		pbap->find_handle = handle;
		ret = phonebook_create_cache(pbap->folder, cache_entry_notify,
						cache_entry_done, pbap);
		goto done;
	}

	id = cache_find(&pbap->cache, handle);
	if (!id) {
		ret = -ENOENT;
		goto fail;
	}

	ret = phonebook_get_entry(pbap->folder, id, pbap->params, query_result,
									pbap);

done:
	if (ret < 0)
		goto fail;

	return pbap;

fail:
	if (err)
		*err = ret;

	return NULL;
}

static ssize_t vobject_pull_read(void *object, void *buf, size_t count,
								uint8_t *hi)
{
	struct pbap_session *pbap = object;

	DBG("buffer %p maxlistcount %d", pbap->buffer,
						pbap->params->maxlistcount);

	if (!pbap->buffer)
		return -EAGAIN;

	/* PhoneBookSize */
	if (pbap->params->maxlistcount == 0)
		*hi = OBEX_HDR_APPARAM;
	else
		/* Stream data */
		*hi = OBEX_HDR_BODY;

	return string_read(pbap->buffer, buf, count);
}

static ssize_t vobject_list_read(void *object, void *buf, size_t count,
								uint8_t *hi)
{
	struct pbap_session *pbap = object;

	DBG("valid %d maxlistcount %d", pbap->cache.valid,
						pbap->params->maxlistcount);

	/* Backend still busy reading contacts */
	if (!pbap->cache.valid)
		return -EAGAIN;

	if (pbap->params->maxlistcount == 0)
		*hi = OBEX_HDR_APPARAM;
	else
		*hi = OBEX_HDR_BODY;

	return string_read(pbap->buffer, buf, count);
}

static ssize_t vobject_vcard_read(void *object, void *buf, size_t count,
								uint8_t *hi)
{
	struct pbap_session *pbap = object;

	DBG("buffer %p", pbap->buffer);

	if (!pbap->buffer)
		return -EAGAIN;

	*hi = OBEX_HDR_BODY;
	return string_read(pbap->buffer, buf, count);
}

static int vobject_close(void *object)
{
	struct pbap_session *pbap = object;

	if (pbap->buffer) {
		string_free(pbap->buffer);
		pbap->buffer = NULL;
	}

	return 0;
}

static struct obex_mime_type_driver mime_pull = {
	.target = PBAP_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/phonebook",
	.open = vobject_pull_open,
	.close = vobject_close,
	.read = vobject_pull_read,
};

static struct obex_mime_type_driver mime_list = {
	.target = PBAP_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/vcard-listing",
	.open = vobject_list_open,
	.close = vobject_close,
	.read = vobject_list_read,
};

static struct obex_mime_type_driver mime_vcard = {
	.target = PBAP_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/vcard",
	.open = vobject_vcard_open,
	.close = vobject_close,
	.read = vobject_vcard_read,
};

static int pbap_init(void)
{
	int err;

	err = phonebook_init();
	if (err < 0)
		goto fail_pb_init;

	err = obex_mime_type_driver_register(&mime_pull);
	if (err < 0)
		goto fail_mime_pull;

	err = obex_mime_type_driver_register(&mime_list);
	if (err < 0)
		goto fail_mime_list;

	err = obex_mime_type_driver_register(&mime_vcard);
	if (err < 0)
		goto fail_mime_vcard;

	err = obex_service_driver_register(&pbap);
	if (err < 0)
			goto fail_pbap_reg;

	return 0;

fail_pbap_reg:
	obex_mime_type_driver_unregister(&mime_vcard);
fail_mime_vcard:
	obex_mime_type_driver_unregister(&mime_list);
fail_mime_list:
	obex_mime_type_driver_unregister(&mime_pull);
fail_mime_pull:
	phonebook_exit();
fail_pb_init:
	return err;
}

static void pbap_exit(void)
{
	obex_service_driver_unregister(&pbap);
	obex_mime_type_driver_unregister(&mime_pull);
	obex_mime_type_driver_unregister(&mime_list);
	obex_mime_type_driver_unregister(&mime_vcard);
	phonebook_exit();
}

OBEX_PLUGIN_DEFINE(pbap, pbap_init, pbap_exit)
