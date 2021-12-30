/*
	JUCI Backend Server

	Copyright (C) 2016 Martin K. Schröder <mkschreder.uk@gmail.com>

	Based on code by: 
	Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
	Copyright (C) 2013-2014 Jo-Philipp Wich <jow@openwrt.org>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <utype/utils.h>
#include <blobpack/blobpack.h>
#include <utype/avl-cmp.h>
#include <utype/list.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <ctype.h>
#include <crypt.h>

#ifdef HAVE_SHADOW
#include <shadow.h>
#else 
#include <sys/types.h>
#include <pwd.h>
#endif


#include "internal.h"
#include "util.h"

#include "orange_session.h"

struct orange_session_data {
	struct avl_node avl;
	struct blob_field *attr;
};

struct orange_session_acl_scope {
	struct avl_node avl;
	struct avl_tree acls;
};

struct orange_session_acl {
	struct avl_node avl;
	const char *object;
	const char *function;
	char *perms; 
	int sort_len;
};

static int _generate_sid(struct orange_sid *sid){
	unsigned char buf[16] = { 0 };
	FILE *f;
	unsigned int i;
	int ret;

	f = fopen("/dev/urandom", "r");
	if (!f)
		return -1;

	ret = fread(buf, 1, sizeof(buf), f);
	fclose(f);

	if (ret < 0)
		return ret;

	for (i = 0; i < sizeof(buf); i++)
		sprintf(&sid->hash[i<<1], "%02x", buf[i]);

	return 0;
}

struct orange_session *orange_session_new(struct orange_user *user, unsigned long long timeout_s){
	struct orange_session *self = calloc(1, sizeof(struct orange_session)); 
	assert(self); 
	
	_generate_sid(&self->sid); 

	self->avl.key = self->sid.hash; 

	avl_init(&self->acl_scopes, avl_strcmp, false, NULL);
	avl_init(&self->data, avl_strcmp, false, NULL);

	self->user = user; 
	timespec_from_now_us(&self->ts_expired, timeout_s * 1000000UL); 
	self->timeout_s = timeout_s; 	
	pthread_mutex_init(&self->lock, NULL); 

	return self; 
}

void orange_session_delete(struct orange_session **_self){
	assert(*_self); 
	struct orange_session *self = *_self; 
	struct orange_session_acl *acl, *nacl;
    struct orange_session_acl_scope *acl_scope, *nacl_scope;
    struct orange_session_data *data, *ndata;

    avl_for_each_element_safe(&self->acl_scopes, acl_scope, avl, nacl_scope) {
        avl_remove_all_elements(&acl_scope->acls, acl, avl, nacl)
            free(acl);

        avl_delete(&self->acl_scopes, &acl_scope->avl);
        free(acl_scope);
    }

    avl_remove_all_elements(&self->data, data, avl, ndata)
        free(data);

	pthread_mutex_destroy(&self->lock); 

	free(self); 
	*_self = NULL; 
}

/*
 * Keys in the AVL tree contain all pattern characters up to the first wildcard.
 * To look up entries, start with the last entry that has a key less than or
 * equal to the method name, then work backwards as long as the AVL key still
 * matches its counterpart in the object name
 */
#define uh_foreach_matching_acl_prefix(_acl, _avl, _obj, _func)     \
    for (_acl = avl_find_le_element(_avl, _obj, _acl, avl);         \
         _acl;                                                      \
         _acl = avl_is_first(_avl, &(_acl)->avl) ? NULL :           \
            avl_prev_element((_acl), avl))

#define uh_foreach_matching_acl(_acl, _avl, _obj, _func)            \
    uh_foreach_matching_acl_prefix(_acl, _avl, _obj, _func)         \
        if (!strncmp((_acl)->object, _obj, (_acl)->sort_len) &&     \
            !fnmatch((_acl)->object, (_obj), FNM_NOESCAPE) &&       \
            !fnmatch((_acl)->function, (_func), FNM_NOESCAPE))


int orange_session_grant(struct orange_session *self, const char *scope, const char *object, const char *function, const char *perm){
    struct orange_session_acl *acl;
    struct orange_session_acl_scope *acl_scope;
    char *new_scope, *new_obj, *new_func, *new_id, *new_perms;
    int id_len;

    if (!object || !function){
        return -EINVAL;
	}

	pthread_mutex_lock(&self->lock); 

    acl_scope = avl_find_element(&self->acl_scopes, scope, acl_scope, avl);

    if (!acl_scope) {
        acl_scope = calloc_a(sizeof(*acl_scope),
                             &new_scope, strlen(scope) + 1);
		
        if (!acl_scope){ 
			pthread_mutex_unlock(&self->lock); 
            return -1;
		}

        acl_scope->avl.key = strcpy(new_scope, scope);
        avl_init(&acl_scope->acls, avl_strcmp, true, NULL);
        avl_insert(&self->acl_scopes, &acl_scope->avl);
    }

    id_len = strcspn(object, "*?[");
    acl = calloc_a(sizeof(*acl),
        &new_obj, strlen(object) + 1,
        &new_func, strlen(function) + 1,
        &new_id, id_len + 1, 
		&new_perms, strlen(perm) + 1);

    if (!acl){
		pthread_mutex_unlock(&self->lock); 
        return -1;
	}

    acl->object = strcpy(new_obj, object);
    acl->function = strcpy(new_func, function);
	acl->perms = strcpy(new_perms, perm);  
    acl->avl.key = strncpy(new_id, object, id_len);
    avl_insert(&acl_scope->acls, &acl->avl);

	pthread_mutex_unlock(&self->lock); 
    return 0;
}

//! New revoke marks permissions as '-' without actually deleting any nodes. 
int orange_session_revoke(struct orange_session *self,
               const char *scope, const char *object, const char *function, const char *perm){
	struct orange_session_acl *acl;
	struct orange_session_acl_scope *acl_scope;

	pthread_mutex_lock(&self->lock); 

	acl_scope = avl_find_element(&self->acl_scopes, scope, acl_scope, avl);
	if (acl_scope) {
		size_t len = strlen(perm); 

		uh_foreach_matching_acl(acl, &acl_scope->acls, object, function){
			for(size_t c = 0; c < len; c++){
				for(size_t j = 0; j < strlen(acl->perms); j++) {
					if(perm[c] == acl->perms[j]) { 
						acl->perms[j] = '-'; 
					}
				}
				
			}
		}
		pthread_mutex_unlock(&self->lock); 
		return 0; 
	}
	
	pthread_mutex_unlock(&self->lock); 
	return -ENOENT;
}

bool orange_session_access(struct orange_session *self, const char *scope, const char *obj, const char *fun, const char *perm){
	struct orange_session_acl *acl;
	struct orange_session_acl_scope *acl_scope;

	pthread_mutex_lock(&self->lock); 

	// update the timeout
	timespec_from_now_us(&self->ts_expired, self->timeout_s * 1000000UL); 

	acl_scope = avl_find_element(&self->acl_scopes, scope, acl_scope, avl);

	if (acl_scope) {
		size_t len = strlen(perm); 
		char *found = alloca(len); 
		memset(found, 0, len); 

		uh_foreach_matching_acl(acl, &acl_scope->acls, obj, fun){
			// check each character in perms and see if it is allowed
			// if no perms are specified then this will always return true
			for(size_t c = 0; c < strlen(perm); c++){
				for(size_t j = 0; j < strlen(acl->perms); j++) {
					if(perm[c] == acl->perms[j]) { 
						found[c] = 1; 
						break; 
					}
				}
				
			}
		}
		for(size_t i = 0; i < len; i++){
			if(!found[i]) {
				pthread_mutex_unlock(&self->lock); 
				return false; 
			}
		}
		pthread_mutex_unlock(&self->lock); 
		return true; 
	}
	
	pthread_mutex_unlock(&self->lock); 
	return false;
}

bool orange_session_expired(struct orange_session *self){
	struct timespec ts_now; 
	timespec_now(&ts_now); 
	pthread_mutex_lock(&self->lock); 
	if(timespec_before(&self->ts_expired, &ts_now)){
		pthread_mutex_unlock(&self->lock); 
		return true; 
	}
	pthread_mutex_unlock(&self->lock); 
	return false; 
}

void orange_session_to_blob(struct orange_session *self, struct blob *buf){
	struct orange_session_acl *acl;
    struct orange_session_acl_scope *acl_scope;

	pthread_mutex_lock(&self->lock); 

	blob_reset(buf); 
	blob_offset_t r = blob_open_table(buf); 
    avl_for_each_element(&self->acl_scopes, acl_scope, avl) {
		blob_put_string(buf, acl_scope->avl.key); 
		blob_offset_t s = blob_open_table(buf); 
        avl_for_each_element(&acl_scope->acls, acl, avl){
			blob_put_string(buf, acl->avl.key); 
			blob_offset_t a = blob_open_table(buf); 
			blob_put_string(buf, "object"); 
			blob_put_string(buf, acl->object); 	
			blob_put_string(buf, "method"); 
			blob_put_string(buf, acl->function); 	
			blob_put_string(buf, "perms"); 
			blob_put_string(buf, acl->perms); 	
			blob_close_table(buf, a); 
		}
		blob_close_table(buf, s); 
    }
	blob_close_table(buf, r); 

	pthread_mutex_unlock(&self->lock); 
}

