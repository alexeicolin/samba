/*
  ldb database library

  Copyright (C) Simo Sorce 2006-2008
  Copyright (C) Nadezhda Ivanova 2010

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 *  Name: ldb
 *
 *  Component: ldb ACL Read module
 *
 *  Description: Module that performs authorisation access checks on read requests
 *               Only DACL checks implemented at this point
 *
 *  Author: Nadezhda Ivanova
 */

#include "includes.h"
#include "ldb_module.h"
#include "auth/auth.h"
#include "libcli/security/security.h"
#include "dsdb/samdb/samdb.h"
#include "librpc/gen_ndr/ndr_security.h"
#include "param/param.h"
#include "dsdb/samdb/ldb_modules/util.h"


struct aclread_context {
	struct ldb_module *module;
	struct ldb_request *req;
	const char * const *attrs;
	const struct dsdb_schema *schema;
	uint32_t sd_flags;
	bool added_nTSecurityDescriptor;
	bool added_instanceType;
	bool added_objectSid;
	bool added_objectClass;
	bool indirsync;

	/* cache on the last parent we checked in this search */
	struct ldb_dn *last_parent_dn;
	int last_parent_check_ret;
};

struct aclread_private {
	bool enabled;

	/* cache of the last SD we read during any search */
	struct security_descriptor *sd_cached;
	struct ldb_val sd_cached_blob;
};

static void aclread_mark_inaccesslible(struct ldb_message_element *el) {
	el->flags |= LDB_FLAG_INTERNAL_INACCESSIBLE_ATTRIBUTE;
}

static bool aclread_is_inaccessible(struct ldb_message_element *el) {
	return el->flags & LDB_FLAG_INTERNAL_INACCESSIBLE_ATTRIBUTE;
}

/*
 * the object has a parent, so we have to check for visibility
 *
 * This helper function uses a per-search cache to avoid checking the
 * parent object for each of many possible children.  This is likely
 * to help on SCOPE_ONE searches and on typical tree structures for
 * SCOPE_SUBTREE, where an OU has many users as children.
 *
 * We rely for safety on the DB being locked for reads during the full
 * search.
 */
static int aclread_check_parent(struct aclread_context *ac,
				struct ldb_message *msg,
				struct ldb_request *req)
{
	int ret;
	struct ldb_dn *parent_dn = NULL;

	/* We may have a cached result from earlier in this search */
	if (ac->last_parent_dn != NULL) {
		/*
		 * We try the no-allocation ldb_dn_compare_base()
		 * first however it will not tell parents and
		 * grand-parents apart
		 */
		int cmp_base = ldb_dn_compare_base(ac->last_parent_dn,
						   msg->dn);
		if (cmp_base == 0) {
			/* Now check if it is a direct parent */
			parent_dn = ldb_dn_get_parent(ac, msg->dn);
			if (parent_dn == NULL) {
				return ldb_oom(ldb_module_get_ctx(ac->module));
			}
			if (ldb_dn_compare(ac->last_parent_dn,
					   parent_dn) == 0) {
				TALLOC_FREE(parent_dn);

				/*
				 * If we checked the same parent last
				 * time, then return the cached
				 * result.
				 *
				 * The cache is valid as long as the
				 * search as the DB is read locked and
				 * the session_info (connected user)
				 * is constant.
				 */
				return ac->last_parent_check_ret;
			}
		}
	}

	{
		TALLOC_CTX *frame = NULL;
		frame = talloc_stackframe();

		/*
		 * This may have been set in the block above, don't
		 * re-parse
		 */
		if (parent_dn == NULL) {
			parent_dn = ldb_dn_get_parent(ac, msg->dn);
			if (parent_dn == NULL) {
				TALLOC_FREE(frame);
				return ldb_oom(ldb_module_get_ctx(ac->module));
			}
		}
		ret = dsdb_module_check_access_on_dn(ac->module,
						     frame,
						     parent_dn,
						     SEC_ADS_LIST,
						     NULL, req);
		talloc_unlink(ac, ac->last_parent_dn);
		ac->last_parent_dn = parent_dn;
		ac->last_parent_check_ret = ret;

		TALLOC_FREE(frame);
	}
	return ret;
}

/*
 * The sd returned from this function is valid until the next call on
 * this module context
 *
 * This helper function uses a cache on the module private data to
 * speed up repeated use of the same SD.
 */

static int aclread_get_sd_from_ldb_message(struct aclread_context *ac,
					   struct ldb_message *acl_res,
					   struct security_descriptor **sd)
{
	struct ldb_message_element *sd_element;
	struct ldb_context *ldb = ldb_module_get_ctx(ac->module);
	struct aclread_private *private_data
		= talloc_get_type(ldb_module_get_private(ac->module),
				  struct aclread_private);
	enum ndr_err_code ndr_err;

	sd_element = ldb_msg_find_element(acl_res, "nTSecurityDescriptor");
	if (sd_element == NULL) {
		return ldb_error(ldb, LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS,
				 "nTSecurityDescriptor is missing");
	}

	if (sd_element->num_values != 1) {
		return ldb_operr(ldb);
	}

	/*
	 * The time spent in ndr_pull_security_descriptor() is quite
	 * expensive, so we check if this is the same binary blob as last
	 * time, and if so return the memory tree from that previous parse.
	 */

	if (private_data->sd_cached != NULL &&
	    private_data->sd_cached_blob.data != NULL &&
	    ldb_val_equal_exact(&sd_element->values[0],
				&private_data->sd_cached_blob)) {
		*sd = private_data->sd_cached;
		return LDB_SUCCESS;
	}

	*sd = talloc(private_data, struct security_descriptor);
	if(!*sd) {
		return ldb_oom(ldb);
	}
	ndr_err = ndr_pull_struct_blob(&sd_element->values[0], *sd, *sd,
			     (ndr_pull_flags_fn_t)ndr_pull_security_descriptor);

	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		TALLOC_FREE(*sd);
		return ldb_operr(ldb);
	}

	talloc_unlink(private_data, private_data->sd_cached_blob.data);
	if (ac->added_nTSecurityDescriptor) {
		private_data->sd_cached_blob = sd_element->values[0];
		talloc_steal(private_data, sd_element->values[0].data);
	} else {
		private_data->sd_cached_blob = ldb_val_dup(private_data,
							   &sd_element->values[0]);
		if (private_data->sd_cached_blob.data == NULL) {
			TALLOC_FREE(*sd);
			return ldb_operr(ldb);
		}
	}

	talloc_unlink(private_data, private_data->sd_cached);
	private_data->sd_cached = *sd;

	return LDB_SUCCESS;
}


static int aclread_callback(struct ldb_request *req, struct ldb_reply *ares)
{
	struct ldb_context *ldb;
	struct aclread_context *ac;
	struct ldb_message *ret_msg;
	struct ldb_message *msg;
	int ret, num_of_attrs = 0;
	unsigned int i, k = 0;
	struct security_descriptor *sd = NULL;
	struct dom_sid *sid = NULL;
	TALLOC_CTX *tmp_ctx;
	uint32_t instanceType;
	const struct dsdb_class *objectclass;

	ac = talloc_get_type(req->context, struct aclread_context);
	ldb = ldb_module_get_ctx(ac->module);
	if (!ares) {
		return ldb_module_done(ac->req, NULL, NULL, LDB_ERR_OPERATIONS_ERROR );
	}
	if (ares->error != LDB_SUCCESS) {
		return ldb_module_done(ac->req, ares->controls,
				       ares->response, ares->error);
	}
	tmp_ctx = talloc_new(ac);
	switch (ares->type) {
	case LDB_REPLY_ENTRY:
		msg = ares->message;
		ret = aclread_get_sd_from_ldb_message(ac, msg, &sd);
		if (ret != LDB_SUCCESS) {
			ldb_debug_set(ldb, LDB_DEBUG_FATAL,
				      "acl_read: cannot get descriptor of %s: %s\n",
				      ldb_dn_get_linearized(msg->dn), ldb_strerror(ret));
			ret = LDB_ERR_OPERATIONS_ERROR;
			goto fail;
		} else if (sd == NULL) {
			ldb_debug_set(ldb, LDB_DEBUG_FATAL,
				      "acl_read: cannot get descriptor of %s (attribute not found)\n",
				      ldb_dn_get_linearized(msg->dn));
			ret = LDB_ERR_OPERATIONS_ERROR;
			goto fail;
		}
		/*
		 * Get the most specific structural object class for the ACL check
		 */
		objectclass = dsdb_get_structural_oc_from_msg(ac->schema, msg);
		if (objectclass == NULL) {
			ldb_asprintf_errstring(ldb, "acl_read: Failed to find a structural class for %s",
					       ldb_dn_get_linearized(msg->dn));
			ret = LDB_ERR_OPERATIONS_ERROR;
			goto fail;
		}

		sid = samdb_result_dom_sid(tmp_ctx, msg, "objectSid");
		/* get the object instance type */
		instanceType = ldb_msg_find_attr_as_uint(msg,
							 "instanceType", 0);
		if (!ldb_dn_is_null(msg->dn) && !(instanceType & INSTANCE_TYPE_IS_NC_HEAD))
		{
			/* the object has a parent, so we have to check for visibility */
			ret = aclread_check_parent(ac, msg, req);
			
			if (ret == LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS) {
				talloc_free(tmp_ctx);
				return LDB_SUCCESS;
			} else if (ret != LDB_SUCCESS) {
				ldb_debug_set(ldb, LDB_DEBUG_FATAL,
					      "acl_read: %s check parent %s - %s\n",
					      ldb_dn_get_linearized(msg->dn),
					      ldb_strerror(ret),
					      ldb_errstring(ldb));
				goto fail;
			}
		}

		/* for every element in the message check RP */
		for (i=0; i < msg->num_elements; i++) {
			const struct dsdb_attribute *attr;
			bool is_sd, is_objectsid, is_instancetype, is_objectclass;
			uint32_t access_mask;
			attr = dsdb_attribute_by_lDAPDisplayName(ac->schema,
								 msg->elements[i].name);
			if (!attr) {
				ldb_debug_set(ldb, LDB_DEBUG_FATAL,
					      "acl_read: %s cannot find attr[%s] in of schema\n",
					      ldb_dn_get_linearized(msg->dn),
					      msg->elements[i].name);
				ret = LDB_ERR_OPERATIONS_ERROR;
				goto fail;
			}
			is_sd = ldb_attr_cmp("nTSecurityDescriptor",
					      msg->elements[i].name) == 0;
			is_objectsid = ldb_attr_cmp("objectSid",
						    msg->elements[i].name) == 0;
			is_instancetype = ldb_attr_cmp("instanceType",
						       msg->elements[i].name) == 0;
			is_objectclass = ldb_attr_cmp("objectClass",
						      msg->elements[i].name) == 0;
			/* these attributes were added to perform access checks and must be removed */
			if (is_objectsid && ac->added_objectSid) {
				aclread_mark_inaccesslible(&msg->elements[i]);
				continue;
			}
			if (is_instancetype && ac->added_instanceType) {
				aclread_mark_inaccesslible(&msg->elements[i]);
				continue;
			}
			if (is_objectclass && ac->added_objectClass) {
				aclread_mark_inaccesslible(&msg->elements[i]);
				continue;
			}
			if (is_sd && ac->added_nTSecurityDescriptor) {
				aclread_mark_inaccesslible(&msg->elements[i]);
				continue;
			}
			/* nTSecurityDescriptor is a special case */
			if (is_sd) {
				access_mask = 0;

				if (ac->sd_flags & (SECINFO_OWNER|SECINFO_GROUP)) {
					access_mask |= SEC_STD_READ_CONTROL;
				}
				if (ac->sd_flags & SECINFO_DACL) {
					access_mask |= SEC_STD_READ_CONTROL;
				}
				if (ac->sd_flags & SECINFO_SACL) {
					access_mask |= SEC_FLAG_SYSTEM_SECURITY;
				}
			} else {
				access_mask = SEC_ADS_READ_PROP;
			}

			if (attr->searchFlags & SEARCH_FLAG_CONFIDENTIAL) {
				access_mask |= SEC_ADS_CONTROL_ACCESS;
			}

			if (access_mask == 0) {
				aclread_mark_inaccesslible(&msg->elements[i]);
				continue;
			}

			ret = acl_check_access_on_attribute(ac->module,
							    tmp_ctx,
							    sd,
							    sid,
							    access_mask,
							    attr,
							    objectclass);

			/*
			 * Dirsync control needs the replpropertymetadata attribute
			 * so return it as it will be removed by the control
			 * in anycase.
			 */
			if (ret == LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS) {
				if (!ac->indirsync) {
					/*
					 * do not return this entry if attribute is
					 * part of the search filter
					 */
					if (dsdb_attr_in_parse_tree(ac->req->op.search.tree,
								msg->elements[i].name)) {
						talloc_free(tmp_ctx);
						return LDB_SUCCESS;
					}
					aclread_mark_inaccesslible(&msg->elements[i]);
				} else {
					/*
					 * We are doing dirysnc answers
					 * and the object shouldn't be returned (normally)
					 * but we will return it without replPropertyMetaData
					 * so that the dirysync module will do what is needed
					 * (remove the object if it is not deleted, or return
					 * just the objectGUID if it's deleted).
					 */
					if (dsdb_attr_in_parse_tree(ac->req->op.search.tree,
								msg->elements[i].name)) {
						ldb_msg_remove_attr(msg, "replPropertyMetaData");
						break;
					} else {
						aclread_mark_inaccesslible(&msg->elements[i]);
					}
				}
			} else if (ret != LDB_SUCCESS) {
				ldb_debug_set(ldb, LDB_DEBUG_FATAL,
					      "acl_read: %s check attr[%s] gives %s - %s\n",
					      ldb_dn_get_linearized(msg->dn),
					      msg->elements[i].name,
					      ldb_strerror(ret),
					      ldb_errstring(ldb));
				goto fail;
			}
		}
		for (i=0; i < msg->num_elements; i++) {
			if (!aclread_is_inaccessible(&msg->elements[i])) {
				num_of_attrs++;
			}
		}
		/*create a new message to return*/
		ret_msg = ldb_msg_new(ac->req);
		ret_msg->dn = msg->dn;
		talloc_steal(ret_msg, msg->dn);
		ret_msg->num_elements = num_of_attrs;
		if (num_of_attrs > 0) {
			ret_msg->elements = talloc_array(ret_msg,
							 struct ldb_message_element,
							 num_of_attrs);
			if (ret_msg->elements == NULL) {
				return ldb_oom(ldb);
			}
			for (i=0; i < msg->num_elements; i++) {
				bool to_remove = aclread_is_inaccessible(&msg->elements[i]);
				if (!to_remove) {
					ret_msg->elements[k] = msg->elements[i];
					talloc_steal(ret_msg->elements, msg->elements[i].name);
					talloc_steal(ret_msg->elements, msg->elements[i].values);
					k++;
				}
			}
			/*
			 * This should not be needed, but some modules
			 * may allocate values on the wrong context...
			 */
			talloc_steal(ret_msg->elements, msg);
		} else {
			ret_msg->elements = NULL;
		}
		talloc_free(tmp_ctx);

		return ldb_module_send_entry(ac->req, ret_msg, ares->controls);
	case LDB_REPLY_REFERRAL:
		return ldb_module_send_referral(ac->req, ares->referral);
	case LDB_REPLY_DONE:
		return ldb_module_done(ac->req, ares->controls,
					ares->response, LDB_SUCCESS);

	}
	return LDB_SUCCESS;
fail:
	talloc_free(tmp_ctx);
	return ldb_module_done(ac->req, NULL, NULL, ret);
}


static int aclread_search(struct ldb_module *module, struct ldb_request *req)
{
	struct ldb_context *ldb;
	int ret;
	struct aclread_context *ac;
	struct ldb_request *down_req;
	struct ldb_control *as_system = ldb_request_get_control(req, LDB_CONTROL_AS_SYSTEM_OID);
	uint32_t flags = ldb_req_get_custom_flags(req);
	struct ldb_result *res;
	struct aclread_private *p;
	bool need_sd = false;
	bool explicit_sd_flags = false;
	bool is_untrusted = ldb_req_is_untrusted(req);
	static const char * const _all_attrs[] = { "*", NULL };
	bool all_attrs = false;
	const char * const *attrs = NULL;
	uint32_t instanceType;
	static const char *acl_attrs[] = {
		"instanceType",
		NULL
	};

	ldb = ldb_module_get_ctx(module);
	p = talloc_get_type(ldb_module_get_private(module), struct aclread_private);

	/* skip access checks if we are system or system control is supplied
	 * or this is not LDAP server request */
	if (!p || !p->enabled ||
	    dsdb_module_am_system(module)
	    || as_system || !is_untrusted) {
		return ldb_next_request(module, req);
	}
	/* no checks on special dn */
	if (ldb_dn_is_special(req->op.search.base)) {
		return ldb_next_request(module, req);
	}

	/* check accessibility of base */
	if (!ldb_dn_is_null(req->op.search.base)) {
		ret = dsdb_module_search_dn(module, req, &res, req->op.search.base,
					    acl_attrs,
					    DSDB_FLAG_NEXT_MODULE |
					    DSDB_FLAG_AS_SYSTEM |
					    DSDB_SEARCH_SHOW_RECYCLED,
					    req);
		if (ret != LDB_SUCCESS) {
			return ldb_error(ldb, ret,
					"acl_read: Error retrieving instanceType for base.");
		}
		instanceType = ldb_msg_find_attr_as_uint(res->msgs[0],
							"instanceType", 0);
		if (instanceType != 0 && !(instanceType & INSTANCE_TYPE_IS_NC_HEAD))
		{
			/* the object has a parent, so we have to check for visibility */
			struct ldb_dn *parent_dn = ldb_dn_get_parent(req, req->op.search.base);
			ret = dsdb_module_check_access_on_dn(module,
							     req,
							     parent_dn,
							     SEC_ADS_LIST,
							     NULL, req);
			if (ret == LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS) {
				return ldb_module_done(req, NULL, NULL, LDB_ERR_NO_SUCH_OBJECT);
			} else if (ret != LDB_SUCCESS) {
				return ldb_module_done(req, NULL, NULL, ret);
			}
		}
	}
	ac = talloc_zero(req, struct aclread_context);
	if (ac == NULL) {
		return ldb_oom(ldb);
	}
	ac->module = module;
	ac->req = req;
	ac->schema = dsdb_get_schema(ldb, req);
	if (flags & DSDB_ACL_CHECKS_DIRSYNC_FLAG) {
		ac->indirsync = true;
	} else {
		ac->indirsync = false;
	}
	if (!ac->schema) {
		return ldb_operr(ldb);
	}

	attrs = req->op.search.attrs;
	if (attrs == NULL) {
		all_attrs = true;
		attrs = _all_attrs;
	} else if (attrs[0] == NULL) {
		all_attrs = true;
		attrs = _all_attrs;
	} else if (ldb_attr_in_list(attrs, "*")) {
		all_attrs = true;
	}

	/*
	 * In theory we should also check for the SD control but control verification is
	 * expensive so we'd better had the ntsecuritydescriptor to the list of
	 * searched attribute and then remove it !
	 */
	ac->sd_flags = dsdb_request_sd_flags(ac->req, &explicit_sd_flags);

	if (ldb_attr_in_list(attrs, "nTSecurityDescriptor")) {
		need_sd = false;
	} else if (explicit_sd_flags && all_attrs) {
		need_sd = false;
	} else {
		need_sd = true;
	}

	if (!all_attrs) {
		if (!ldb_attr_in_list(attrs, "instanceType")) {
			attrs = ldb_attr_list_copy_add(ac, attrs, "instanceType");
			if (attrs == NULL) {
				return ldb_oom(ldb);
			}
			ac->added_instanceType = true;
		}
		if (!ldb_attr_in_list(req->op.search.attrs, "objectSid")) {
			attrs = ldb_attr_list_copy_add(ac, attrs, "objectSid");
			if (attrs == NULL) {
				return ldb_oom(ldb);
			}
			ac->added_objectSid = true;
		}
		if (!ldb_attr_in_list(req->op.search.attrs, "objectClass")) {
			attrs = ldb_attr_list_copy_add(ac, attrs, "objectClass");
			if (attrs == NULL) {
				return ldb_oom(ldb);
			}
			ac->added_objectClass = true;
		}
	}

	if (need_sd) {
		attrs = ldb_attr_list_copy_add(ac, attrs, "nTSecurityDescriptor");
		if (attrs == NULL) {
			return ldb_oom(ldb);
		}
		ac->added_nTSecurityDescriptor = true;
	}

	ac->attrs = req->op.search.attrs;
	ret = ldb_build_search_req_ex(&down_req,
				      ldb, ac,
				      req->op.search.base,
				      req->op.search.scope,
				      req->op.search.tree,
				      attrs,
				      req->controls,
				      ac, aclread_callback,
				      req);

	if (ret != LDB_SUCCESS) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	return ldb_next_request(module, down_req);
}

static int aclread_init(struct ldb_module *module)
{
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	struct aclread_private *p = talloc_zero(module, struct aclread_private);
	if (p == NULL) {
		return ldb_module_oom(module);
	}
	p->enabled = lpcfg_parm_bool(ldb_get_opaque(ldb, "loadparm"), NULL, "acl", "search", true);
	ldb_module_set_private(module, p);
	return ldb_next_init(module);
}

static const struct ldb_module_ops ldb_aclread_module_ops = {
	.name		   = "aclread",
	.search            = aclread_search,
	.init_context      = aclread_init
};

int ldb_aclread_module_init(const char *version)
{
	LDB_MODULE_CHECK_VERSION(version);
	return ldb_register_module(&ldb_aclread_module_ops);
}
