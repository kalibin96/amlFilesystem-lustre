/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001-2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.sf.net/projects/lustre/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDC

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/pagemap.h>
# include <linux/miscdevice.h>
# include <linux/init.h>
#else
# include <liblustre.h>
#endif

#include <linux/obd_class.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_sec.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_acl.h>
#include "mdc_internal.h"

#define REQUEST_MINOR 244

static int mdc_cleanup(struct obd_device *obd, int flags);

int mdc_get_secdesc_size(void)
{
#ifdef __KERNEL__
        int ngroups = current_ngroups;

        if (ngroups > LUSTRE_MAX_GROUPS)
                ngroups = LUSTRE_MAX_GROUPS;

        return sizeof(struct mds_req_sec_desc) +
                sizeof(__u32) * ngroups;
#else
        return 0;
#endif
}

/*
 * because group info might have changed since last time we call
 * get_secdesc_size(), so here we did more sanity check to prevent garbage gids
 */
void mdc_pack_secdesc(struct ptlrpc_request *req, int size)
{
#ifdef __KERNEL__
        struct mds_req_sec_desc *rsd;
        
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,4)
        struct group_info *ginfo;
#endif

        rsd = lustre_msg_buf(req->rq_reqmsg,
                             MDS_REQ_SECDESC_OFF, size);
        
        rsd->rsd_uid = current->uid;
        rsd->rsd_gid = current->gid;
        rsd->rsd_fsuid = current->fsuid;
        rsd->rsd_fsgid = current->fsgid;
        rsd->rsd_cap = current->cap_effective;
        rsd->rsd_ngroups = (size - sizeof(*rsd)) / sizeof(__u32);
        LASSERT(rsd->rsd_ngroups <= LUSTRE_MAX_GROUPS);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,4)
        task_lock(current);
        get_group_info(current->group_info);
        ginfo = current->group_info;
        task_unlock(current);
        if (rsd->rsd_ngroups > ginfo->ngroups)
                rsd->rsd_ngroups = ginfo->ngroups;
        memcpy(rsd->rsd_groups, ginfo->blocks[0],
               rsd->rsd_ngroups * sizeof(__u32));
#else
        LASSERT(rsd->rsd_ngroups <= NGROUPS);
        if (rsd->rsd_ngroups > current->ngroups)
                rsd->rsd_ngroups = current->ngroups;
        memcpy(rsd->rsd_groups, current->groups,
               rsd->rsd_ngroups * sizeof(__u32));
#endif
#endif
}

extern int mds_queue_req(struct ptlrpc_request *);
/* Helper that implements most of mdc_getstatus and signal_completed_replay. */
/* XXX this should become mdc_get_info("key"), sending MDS_GET_INFO RPC */
static int send_getstatus(struct obd_import *imp, struct lustre_id *rootid,
                          int level, int msg_flags)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[2] = {0, sizeof(*body)};
        ENTRY;

        //size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(imp, LUSTRE_MDS_VERSION, MDS_GETSTATUS,
                              2, size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        //mdc_pack_secdesc(req, size[0]);

        body = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF, sizeof (*body));
        req->rq_send_state = level;
        req->rq_replen = lustre_msg_size(1, &size[1]);

        req->rq_reqmsg->flags |= msg_flags;
        rc = ptlrpc_queue_wait(req);

        if (!rc) {
                body = lustre_swab_repbuf (req, 0, sizeof (*body),
                                           lustre_swab_mds_body);
                if (body == NULL) {
                        CERROR ("Can't extract mds_body\n");
                        GOTO (out, rc = -EPROTO);
                }

                memcpy(rootid, &body->id1, sizeof(*rootid));

                CDEBUG(D_NET, "root ino="LPU64", last_committed="LPU64
                       ", last_xid="LPU64"\n", rootid->li_stc.u.e3s.l3s_ino,
                       req->rq_repmsg->last_committed, req->rq_repmsg->last_xid);
        }

        EXIT;
 out:
        ptlrpc_req_finished(req);
        return rc;
}

/* This should be mdc_get_info("rootid") */
int mdc_getstatus(struct obd_export *exp, struct lustre_id *rootid)
{
        return send_getstatus(class_exp2cliimp(exp), rootid,
                              LUSTRE_IMP_FULL, 0);
}

int mdc_getattr_common(struct obd_export *exp, unsigned int ea_size,
                       struct ptlrpc_request *req)
{
        struct mds_body *body, *reqbody;
        void            *eadata;
        int              rc;
        int              repsize[4] = {sizeof(*body)};
        int              bufcount = 1;
        ENTRY;

        /* request message already built */

        if (ea_size != 0) {
                repsize[bufcount++] = ea_size;
                CDEBUG(D_INODE, "reserved %u bytes for MD/symlink in packet\n",
                       ea_size);
        }

        reqbody = lustre_msg_buf(req->rq_reqmsg, 1, sizeof(*reqbody));

        if (reqbody->valid & OBD_MD_FLACL_ACCESS) {
                repsize[bufcount++] = 4;
                repsize[bufcount++] = xattr_acl_size(LL_ACL_MAX_ENTRIES);
        }

        req->rq_replen = lustre_msg_size(bufcount, repsize);

        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        if (rc != 0)
                RETURN (rc);

        body = lustre_swab_repbuf (req, 0, sizeof (*body),
                                   lustre_swab_mds_body);
        if (body == NULL) {
                CERROR ("Can't unpack mds_body\n");
                RETURN (-EPROTO);
        }

        CDEBUG(D_NET, "mode: %o\n", body->mode);

        LASSERT_REPSWAB (req, 1);

        /* Skip the check if getxattr/listxattr are called with no buffers */
        if ((reqbody->valid & (OBD_MD_FLEA | OBD_MD_FLEALIST)) &&
            (reqbody->eadatasize != 0)){
                if (body->eadatasize != 0) {
                /* reply indicates presence of eadata; check it's there... */
                        eadata = lustre_msg_buf (req->rq_repmsg, 1,
                                                 body->eadatasize);
                        if (eadata == NULL) {
                                CERROR ("Missing/short eadata\n");
                                RETURN (-EPROTO);
                        }
                 }
         }

        RETURN (0);
}

int mdc_getattr(struct obd_export *exp, struct lustre_id *id,
                __u64 valid, const char *ea_name, int ea_namelen,
                unsigned int ea_size, struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int bufcount = 2;
        int size[3] = {0, sizeof(*body)};
        int rc;
        ENTRY;

        /* XXX do we need to make another request here?  We just did a getattr
         *     to do the lookup in the first place.
         */
        size[0] = mdc_get_secdesc_size();

        LASSERT((ea_name != NULL) == (ea_namelen != 0));
        if (valid & (OBD_MD_FLEA | OBD_MD_FLEALIST)) {
                size[bufcount] = ea_namelen;
                bufcount++;
        }

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_GETATTR, bufcount, size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        mdc_pack_secdesc(req, size[0]);

        body = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF, sizeof (*body));
        memcpy(&body->id1, id, sizeof(*id));
        body->valid = valid;
        body->eadatasize = ea_size;


        if (valid & OBD_MD_FLEA) {
                LASSERT(strnlen(ea_name, ea_namelen) == (ea_namelen - 1));
                memcpy(lustre_msg_buf(req->rq_reqmsg, 2, ea_namelen),
                       ea_name, ea_namelen);
        }

        rc = mdc_getattr_common(exp, ea_size, req);
        if (rc != 0) {
                ptlrpc_req_finished (req);
                req = NULL;
        }
 out:
        *request = req;
        RETURN (rc);
}

int mdc_getattr_lock(struct obd_export *exp, struct lustre_id *id,
                     char *filename, int namelen, __u64 valid,
                     unsigned int ea_size, struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[3] = {0, sizeof(*body), namelen};
        ENTRY;

        size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_GETATTR_LOCK, 3, size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        mdc_pack_secdesc(req, size[0]);

        body = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF, sizeof (*body));
        memcpy(&body->id1, id, sizeof(*id));
        body->valid = valid;
        body->eadatasize = ea_size;

        if (filename != NULL) {
                LASSERT (strnlen (filename, namelen) == namelen - 1);
                memcpy(lustre_msg_buf(req->rq_reqmsg, 2, namelen),
                       filename, namelen);
        } else {
                LASSERT(namelen == 1);
        }

        rc = mdc_getattr_common(exp, ea_size, req);
        if (rc != 0) {
                ptlrpc_req_finished (req);
                req = NULL;
        }
 out:
        *request = req;
        RETURN(rc);
}

/* This should be called with both the request and the reply still packed. */
int mdc_store_inode_generation(struct obd_export *exp,
                               struct ptlrpc_request *req,
                               int reqoff, int repoff)
{
        struct mds_rec_create *rec =
                lustre_msg_buf(req->rq_reqmsg, reqoff, sizeof(*rec));
        struct mds_body *body =
                lustre_msg_buf(req->rq_repmsg, repoff, sizeof(*body));

        LASSERT (rec != NULL);
        LASSERT (body != NULL);

        memcpy(&rec->cr_replayid, &body->id1, sizeof(rec->cr_replayid));
        DEBUG_REQ(D_HA, req, "storing generation for ino "DLID4,
                  OLID4(&rec->cr_replayid));
        return 0;
}

int mdc_req2lustre_md(struct obd_export *exp_lmv, struct ptlrpc_request *req, 
                      unsigned int offset, struct obd_export *exp_lov, 
                      struct lustre_md *md)
{
        void *buf;
        int size, acl_off;
        struct posix_acl *acl;
        int rc = 0;
        ENTRY;

        LASSERT(md);
        memset(md, 0, sizeof(*md));

        md->body = lustre_msg_buf(req->rq_repmsg, offset, sizeof (*md->body));
        LASSERT (md->body != NULL);
        LASSERT_REPSWABBED (req, offset);

        if (!(md->body->valid & OBD_MD_FLEASIZE) && 
            !(md->body->valid & OBD_MD_FLDIREA))
                RETURN(0);

        /* ea is presented in reply, parse it */
        if (S_ISREG(md->body->mode)) {
                int lmmsize;
                struct lov_mds_md *lmm;

                if (md->body->eadatasize == 0) {
                        CERROR ("OBD_MD_FLEASIZE set, but eadatasize 0\n");
                        RETURN(-EPROTO);
                }
                lmmsize = md->body->eadatasize;
                lmm = lustre_msg_buf(req->rq_repmsg, offset + 1, lmmsize);
                LASSERT (lmm != NULL);
                LASSERT_REPSWABBED (req, offset + 1);

                rc = obd_unpackmd(exp_lov, &md->lsm, lmm, lmmsize);
                if (rc >= 0) {
                        LASSERT (rc >= sizeof (*md->lsm));
                        rc = 0;
                }
        } else if (S_ISDIR(md->body->mode)) {
                struct mea *mea;
                int mdsize;
                LASSERT(exp_lmv != NULL);
                
                /* dir can be non-splitted */
                if (md->body->eadatasize == 0)
                        RETURN(0);

                mdsize = md->body->eadatasize;
                mea = lustre_msg_buf(req->rq_repmsg, offset + 1, mdsize);
                LASSERT(mea != NULL);

                /* 
                 * check mea for validness, as there is possible that old tests
                 * will try to set lov EA to dir object and thus confuse this
                 * stuff.
                 */
                if (mea->mea_magic != MEA_MAGIC_LAST_CHAR &&
                    mea->mea_magic != MEA_MAGIC_ALL_CHARS)
                        GOTO(out_invalid_mea, rc = -EINVAL);

                if (mea->mea_count > 256 || mea->mea_master > 256 ||
                    mea->mea_master > mea->mea_count)
                        GOTO(out_invalid_mea, rc = -EINVAL);
                        
                LASSERT(id_fid(&mea->mea_ids[0]));

                rc = obd_unpackmd(exp_lmv, (void *)&md->mea,
                                  (void *)mea, mdsize);
                if (rc >= 0) {
                        LASSERT (rc >= sizeof (*md->mea));
                        rc = 0;
                }

                RETURN(rc);

        out_invalid_mea:
                CERROR("Detected invalid mea, which does not "
                       "support neither old either new format.\n");
        } else {
                LASSERT(S_ISCHR(md->body->mode) ||
                        S_ISBLK(md->body->mode) ||
                        S_ISFIFO(md->body->mode)||
                        S_ISLNK(md->body->mode) ||
                        S_ISSOCK(md->body->mode));
        }

        acl_off = (md->body->valid & OBD_MD_FLEASIZE) ? (offset + 2) :
                  (offset + 1);

        if (md->body->valid & OBD_MD_FLACL_ACCESS) {
                size = le32_to_cpu(*(__u32 *) lustre_msg_buf(req->rq_repmsg, 
                                   acl_off, 4));
                buf = lustre_msg_buf(req->rq_repmsg, acl_off + 1, size);

                acl = posix_acl_from_xattr(buf, size);
                if (IS_ERR(acl)) {
                        rc = PTR_ERR(acl);
                        CERROR("convert xattr to acl failed: %d\n", rc);
                        RETURN(rc);
                } else if (acl) {
                        rc = posix_acl_valid(acl);
                        if (rc) {
                                CERROR("acl valid error: %d\n", rc);
                                posix_acl_release(acl);
                                RETURN(rc);
                        }
                }

                md->acl_access = acl;
        }

        RETURN(rc);
}

static void mdc_commit_open(struct ptlrpc_request *req)
{
        struct mdc_open_data *mod = req->rq_cb_data;
        if (mod == NULL)
                return;

        if (mod->mod_close_req != NULL)
                mod->mod_close_req->rq_cb_data = NULL;

        if (mod->mod_och != NULL)
                mod->mod_och->och_mod = NULL;

        OBD_FREE(mod, sizeof(*mod));
        req->rq_cb_data = NULL;
}

static void mdc_replay_open(struct ptlrpc_request *req)
{
        struct mdc_open_data *mod = req->rq_cb_data;
        struct obd_client_handle *och;
        struct ptlrpc_request *close_req;
        struct lustre_handle old;
        struct mds_body *body;
        ENTRY;

        body = lustre_swab_repbuf(req, 1, sizeof(*body), lustre_swab_mds_body);
        LASSERT (body != NULL);

        if (mod == NULL) {
                DEBUG_REQ(D_ERROR, req,
                          "can't properly replay without open data");
                EXIT;
                return;
        }

        och = mod->mod_och;
        if (och != NULL) {
                struct lustre_handle *file_fh;
                LASSERT(och->och_magic == OBD_CLIENT_HANDLE_MAGIC);
                file_fh = &och->och_fh;
                CDEBUG(D_HA, "updating handle from "LPX64" to "LPX64"\n",
                       file_fh->cookie, body->handle.cookie);
                memcpy(&old, file_fh, sizeof(old));
                memcpy(file_fh, &body->handle, sizeof(*file_fh));
        }

        close_req = mod->mod_close_req;
        if (close_req != NULL) {
                struct mds_body *close_body;
                LASSERT(close_req->rq_reqmsg->opc == MDS_CLOSE);
                close_body = lustre_msg_buf(close_req->rq_reqmsg,
                                            MDS_REQ_REC_OFF,
                                            sizeof(*close_body));
                if (och != NULL)
                        LASSERT(!memcmp(&old, &close_body->handle, sizeof old));
                DEBUG_REQ(D_HA, close_req, "updating close body with new fh");
                memcpy(&close_body->handle, &body->handle,
                       sizeof(close_body->handle));
        }

        EXIT;
}

int  mdc_set_open_replay_data(struct obd_export *exp,
                              struct obd_client_handle *och,
                              struct ptlrpc_request *open_req)
{
        struct mdc_open_data *mod;
        struct mds_rec_create *rec;
        struct mds_body *body;

        rec = lustre_msg_buf(open_req->rq_reqmsg, MDS_REQ_INTENT_REC_OFF,
                             sizeof(*rec));
        body = lustre_msg_buf(open_req->rq_repmsg, 1, sizeof(*body));

        LASSERT(rec != NULL);
        /* outgoing messages always in my byte order */
        LASSERT(body != NULL);
        /* incoming message in my byte order (it's been swabbed) */
        LASSERT_REPSWABBED(open_req, 1);

        OBD_ALLOC(mod, sizeof(*mod));
        if (mod == NULL) {
                DEBUG_REQ(D_ERROR, open_req, "can't allocate mdc_open_data");
                return 0;
        }

        och->och_mod = mod;
        mod->mod_och = och;
        mod->mod_open_req = open_req;

        memcpy(&rec->cr_replayid, &body->id1, sizeof rec->cr_replayid);
        open_req->rq_replay_cb = mdc_replay_open;
        open_req->rq_commit_cb = mdc_commit_open;
        open_req->rq_cb_data = mod;
        DEBUG_REQ(D_HA, open_req, "set up replay data");
        return 0;
}

int mdc_clear_open_replay_data(struct obd_export *exp,
                               struct obd_client_handle *och)
{
        struct mdc_open_data *mod = och->och_mod;

        /* Don't free the structure now (it happens in mdc_commit_open, after
         * we're sure we won't need to fix up the close request in the future),
         * but make sure that replay doesn't poke at the och, which is about to
         * be freed. */
        LASSERT(mod != LP_POISON);
        if (mod != NULL)
                mod->mod_och = NULL;
        och->och_mod = NULL;
        return 0;
}

static void mdc_commit_close(struct ptlrpc_request *req)
{
        struct mdc_open_data *mod = req->rq_cb_data;
        struct ptlrpc_request *open_req;
        struct obd_import *imp = req->rq_import;

        DEBUG_REQ(D_HA, req, "close req committed");
        if (mod == NULL)
                return;

        mod->mod_close_req = NULL;
        req->rq_cb_data = NULL;
        req->rq_commit_cb = NULL;

        open_req = mod->mod_open_req;
        LASSERT(open_req != NULL);
        LASSERT(open_req != LP_POISON);
        LASSERT(open_req->rq_type != LI_POISON);

        DEBUG_REQ(D_HA, open_req, "open req balanced");
        if (open_req->rq_transno == 0) {
                DEBUG_REQ(D_ERROR, open_req, "BUG 3892  open");
                DEBUG_REQ(D_ERROR, req, "BUG 3892 close");
                LASSERTF(open_req->rq_transno != 0, "BUG 3892");
        }
        LASSERT(open_req->rq_import == imp);

        /* We no longer want to preserve this for transno-unconditional
         * replay. */
        spin_lock(&open_req->rq_lock);
        open_req->rq_replay = 0;
        spin_unlock(&open_req->rq_lock);
}

static int mdc_close_interpret(struct ptlrpc_request *req, void *data, int rc)
{
        union ptlrpc_async_args *aa = data;
        struct mdc_rpc_lock *rpc_lock;
        struct obd_device *obd = aa->pointer_arg[1];
        unsigned long flags;

        spin_lock_irqsave(&req->rq_lock, flags);
        rpc_lock = aa->pointer_arg[0];
        aa->pointer_arg[0] = NULL;
        spin_unlock_irqrestore (&req->rq_lock, flags);

        if (rpc_lock == NULL) {
                CERROR("called with NULL rpc_lock\n");
        } else {
                mdc_put_rpc_lock(rpc_lock, NULL);
                LASSERTF(rpc_lock == obd->u.cli.cl_rpc_lock, "%p != %p\n",
                         rpc_lock, obd->u.cli.cl_rpc_lock);
        }
        wake_up(&req->rq_reply_waitq);
        RETURN(rc);
}

/* We can't use ptlrpc_check_reply, because we don't want to wake up for
 * anything but a reply or an error. */
static int mdc_close_check_reply(struct ptlrpc_request *req)
{
        int rc = 0;
        unsigned long flags;

        spin_lock_irqsave(&req->rq_lock, flags);
        if (req->rq_async_args.pointer_arg[0] == NULL)
                rc = 1;
        spin_unlock_irqrestore (&req->rq_lock, flags);
        return rc;
}

static int go_back_to_sleep(void *unused)
{
        return 0;
}

int mdc_close(struct obd_export *exp, struct obdo *oa,
              struct obd_client_handle *och, struct ptlrpc_request **request)
{
        struct obd_device *obd = class_exp2obd(exp);
        int reqsize[3] = {0, sizeof(struct mds_body),
                          obd->u.cli.cl_max_mds_cookiesize};
        int rc, repsize[3] = {sizeof(struct mds_body),
                              obd->u.cli.cl_max_mds_easize,
                              obd->u.cli.cl_max_mds_cookiesize};
        struct ptlrpc_request *req;
        struct mdc_open_data *mod;
        struct l_wait_info lwi;
        ENTRY;

        //reqsize[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_CLOSE, 3, reqsize, NULL);
        if (req == NULL)
                GOTO(out, rc = -ENOMEM);

        //mdc_pack_secdesc(req, reqsize[0]);

        /* Ensure that this close's handle is fixed up during replay. */
        LASSERT(och != NULL);
        mod = och->och_mod;
        if (likely(mod != NULL)) {
                mod->mod_close_req = req;
                LASSERT(mod->mod_open_req->rq_type != LI_POISON);
                DEBUG_REQ(D_HA, mod->mod_open_req, "matched open req %p",
                          mod->mod_open_req);
        } else {
                CDEBUG(D_HA, "couldn't find open req; expecting close error\n");
        }

        mdc_close_pack(req, 1, oa, oa->o_valid, och);

        req->rq_replen = lustre_msg_size(3, repsize);
        req->rq_commit_cb = mdc_commit_close;
        LASSERT(req->rq_cb_data == NULL);
        req->rq_cb_data = mod;

        /* We hand a ref to the rpcd here, so we need another one of our own. */
        ptlrpc_request_addref(req);

        mdc_get_rpc_lock(obd->u.cli.cl_rpc_lock, NULL);
        req->rq_interpret_reply = mdc_close_interpret;
        req->rq_async_args.pointer_arg[0] = obd->u.cli.cl_rpc_lock;
        req->rq_async_args.pointer_arg[1] = obd;
        ptlrpcd_add_req(req);
        lwi = LWI_TIMEOUT_INTR(MAX(req->rq_timeout * HZ, 1), go_back_to_sleep,
                               NULL, NULL);
        rc = l_wait_event(req->rq_reply_waitq, mdc_close_check_reply(req),
                          &lwi);
        if (req->rq_repmsg == NULL) {
                CDEBUG(D_HA, "request failed to send: %p, %d\n", req,
                       req->rq_status);
                if (rc == 0)
                        rc = req->rq_status ? req->rq_status : -EIO;
        } else if (rc == 0) {
                rc = req->rq_repmsg->status;
                if (req->rq_repmsg->type == PTL_RPC_MSG_ERR) {
                        DEBUG_REQ(D_ERROR, req, "type == PTL_RPC_MSG_ERR, err "
                                  "= %d", rc);
                        if (rc > 0)
                                rc = -rc;
                } else if (mod == NULL) {
                        CERROR("Unexpected: can't find mdc_open_data, but the "
                               "close succeeded.  Please tell CFS.\n");
                }
                if (!lustre_swab_repbuf(req, 0, sizeof(struct mds_body),
                                        lustre_swab_mds_body)) {
                        CERROR("Error unpacking mds_body\n");
                        rc = -EPROTO;
                }
        }
        if (req->rq_async_args.pointer_arg[0] != NULL) {
                CERROR("returned without dropping rpc_lock: rc %d\n", rc);
                mdc_close_interpret(req, &req->rq_async_args, rc);
        }

        EXIT;
 out:
        *request = req;
        return rc;
}

int mdc_done_writing(struct obd_export *exp, struct obdo *obdo)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[2] = {0, sizeof(*body)};
        ENTRY;

        size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_DONE_WRITING, 2, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        mdc_pack_secdesc(req, size[0]);

        body = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF, 
                              sizeof(*body));
        
        mdc_pack_id(&body->id1, obdo->o_id, 0, obdo->o_mode, 
                    obdo->o_mds, obdo->o_fid);
        
        body->size = obdo->o_size;
        body->blocks = obdo->o_blocks;
        body->flags = obdo->o_flags;
        body->valid = obdo->o_valid;

        req->rq_replen = lustre_msg_size(1, &size[1]);

        rc = ptlrpc_queue_wait(req);
        ptlrpc_req_finished(req);
        RETURN(rc);
}

int mdc_readpage(struct obd_export *exp,
                 struct lustre_id *id,
                 __u64 offset, struct page *page,
                 struct ptlrpc_request **request)
{
        struct obd_import *imp = class_exp2cliimp(exp);
        struct ptlrpc_request *req = NULL;
        struct ptlrpc_bulk_desc *desc = NULL;
        struct mds_body *body;
        int rc, size[2] = {0, sizeof(*body)};
        ENTRY;

        CDEBUG(D_INODE, "inode: %ld\n", (long)id->li_stc.u.e3s.l3s_ino);

        size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(imp, LUSTRE_MDS_VERSION, MDS_READPAGE,
                              2, size, NULL);
        if (req == NULL)
                GOTO(out, rc = -ENOMEM);
        /* XXX FIXME bug 249 */
        req->rq_request_portal = MDS_READPAGE_PORTAL;

        mdc_pack_secdesc(req, size[0]);

        desc = ptlrpc_prep_bulk_imp(req, 1, BULK_PUT_SINK, MDS_BULK_PORTAL);
        if (desc == NULL)
                GOTO(out, rc = -ENOMEM);
        /* NB req now owns desc and will free it when it gets freed */

        ptlrpc_prep_bulk_page(desc, page, 0, PAGE_CACHE_SIZE);
        mdc_readdir_pack(req, 1, offset, PAGE_CACHE_SIZE, id);

        req->rq_replen = lustre_msg_size(1, &size[1]);
        rc = ptlrpc_queue_wait(req);

        if (rc == 0) {
                body = lustre_swab_repbuf(req, 0, sizeof (*body),
                                          lustre_swab_mds_body);
                if (body == NULL) {
                        CERROR("Can't unpack mds_body\n");
                        GOTO(out, rc = -EPROTO);
                }

                if (req->rq_bulk->bd_nob_transferred != PAGE_CACHE_SIZE) {
                        CERROR ("Unexpected # bytes transferred: %d"
                                " (%ld expected)\n",
                                req->rq_bulk->bd_nob_transferred,
                                PAGE_CACHE_SIZE);
                        GOTO (out, rc = -EPROTO);
                }
        }

        EXIT;
 out:
        *request = req;
        return rc;
}

static int mdc_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                         void *karg, void *uarg)
{
        struct obd_device *obd = exp->exp_obd;
        struct obd_ioctl_data *data = karg;
        struct obd_import *imp = obd->u.cli.cl_import;
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        MOD_INC_USE_COUNT;
#else
        if (!try_module_get(THIS_MODULE)) {
                CERROR("Can't get module. Is it alive?");
                return -EINVAL;
        }
#endif
        switch (cmd) {
        case OBD_IOC_CLIENT_RECOVER:
                rc = ptlrpc_recover_import(imp, data->ioc_inlbuf1);
                if (rc < 0)
                        GOTO(out, rc);
                GOTO(out, rc = 0);
        case IOC_OSC_SET_ACTIVE:
                rc = ptlrpc_set_import_active(imp, data->ioc_offset);
                GOTO(out, rc);
        case OBD_IOC_PARSE: {
                ctxt = llog_get_context(&exp->exp_obd->obd_llogs,
                                        LLOG_CONFIG_REPL_CTXT);
                rc = class_config_process_llog(ctxt, data->ioc_inlbuf1, NULL);
                GOTO(out, rc);
        }
#ifdef __KERNEL__
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                ctxt = llog_get_context(&obd->obd_llogs, LLOG_CONFIG_REPL_CTXT);
                rc = llog_ioctl(ctxt, cmd, data);

                GOTO(out, rc);
        }
#endif
        default:
                CERROR("mdc_ioctl(): unrecognised ioctl %#x\n", cmd);
                GOTO(out, rc = -ENOTTY);
        }
out:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        MOD_DEC_USE_COUNT;
#else
        module_put(THIS_MODULE);
#endif

        return rc;
}

int mdc_set_info(struct obd_export *exp, obd_count keylen,
                 void *key, obd_count vallen, void *val)
{
        int rc = -EINVAL;

        if (keylen == strlen("initial_recov") &&
            memcmp(key, "initial_recov", strlen("initial_recov")) == 0) {
                struct obd_import *imp = exp->exp_obd->u.cli.cl_import;
                if (vallen != sizeof(int))
                        RETURN(-EINVAL);
                imp->imp_initial_recov = *(int *)val;
                CDEBUG(D_HA, "%s: set imp_no_init_recov = %d\n",
                       exp->exp_obd->obd_name,
                       imp->imp_initial_recov);
                RETURN(0);
        } else if (keylen >= strlen("mds_type") && strcmp(key, "mds_type") == 0) {
                struct ptlrpc_request *req;
                char *bufs[2] = {key, val};
                int rc, size[2] = {keylen, vallen};

                req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_OBD_VERSION,
                                      OST_SET_INFO, 2, size, bufs);
                if (req == NULL)
                        RETURN(-ENOMEM);

                req->rq_replen = lustre_msg_size(0, NULL);
                rc = ptlrpc_queue_wait(req);
                ptlrpc_req_finished(req);
                RETURN(rc);
        } else if (keylen >= strlen("inter_mds") && strcmp(key, "inter_mds") == 0) {
                struct obd_import *imp = class_exp2cliimp(exp);
                imp->imp_server_timeout = 1;
                CDEBUG(D_OTHER, "%s: timeout / 2\n", exp->exp_obd->obd_name);
                RETURN(0);
        } else if (keylen == strlen("sec") && memcmp(key, "sec", keylen) == 0) {
                struct client_obd *cli = &exp->exp_obd->u.cli;

                if (vallen == strlen("null") &&
                    memcmp(val, "null", vallen) == 0) {
                        cli->cl_sec_flavor = PTLRPC_SEC_NULL;
                        cli->cl_sec_subflavor = 0;
                        RETURN(0);
                }
                if (vallen == strlen("krb5i") &&
                    memcmp(val, "krb5i", vallen) == 0) {
                        cli->cl_sec_flavor = PTLRPC_SEC_GSS;
                        cli->cl_sec_subflavor = PTLRPC_SEC_GSS_KRB5I;
                        RETURN(0);
                }
                if (vallen == strlen("krb5p") &&
                    memcmp(val, "krb5p", vallen) == 0) {
                        cli->cl_sec_flavor = PTLRPC_SEC_GSS;
                        cli->cl_sec_subflavor = PTLRPC_SEC_GSS_KRB5P;
                        RETURN(0);
                }
                CERROR("unrecognized security type %s\n", (char*) val);
                rc = -EINVAL;
        } else if (keylen == strlen("nllu") && memcmp(key, "nllu", keylen) == 0) {
                struct client_obd *cli = &exp->exp_obd->u.cli;

                LASSERT(vallen == sizeof(__u32) * 2);
                cli->cl_nllu = ((__u32 *) val)[0];
                cli->cl_nllg = ((__u32 *) val)[1];
                RETURN(0);
        }

        RETURN(rc);
}

static int mdc_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                      unsigned long max_age)
{
        struct obd_statfs *msfs;
        struct ptlrpc_request *req;
        int rc, size = sizeof(*msfs);
        ENTRY;

        /* We could possibly pass max_age in the request (as an absolute
         * timestamp or a "seconds.usec ago") so the target can avoid doing
         * extra calls into the filesystem if that isn't necessary (e.g.
         * during mount that would help a bit).  Having relative timestamps
         * is not so great if request processing is slow, while absolute
         * timestamps are not ideal because they need time synchronization. */
        req = ptlrpc_prep_req(obd->u.cli.cl_import, LUSTRE_MDS_VERSION,
                              MDS_STATFS, 0, NULL, NULL);
        if (!req)
                RETURN(-ENOMEM);

        req->rq_replen = lustre_msg_size(1, &size);

        mdc_get_rpc_lock(obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(obd->u.cli.cl_rpc_lock, NULL);

        if (rc) {
                /* this can be LMV fake import, whcih is not connected. */
                if (!req->rq_import->imp_connection)
                        memset(osfs, 0, sizeof(*osfs));
                GOTO(out, rc);
        }

        msfs = lustre_swab_repbuf(req, 0, sizeof(*msfs),
                                  lustre_swab_obd_statfs);
        if (msfs == NULL) {
                CERROR("Can't unpack obd_statfs\n");
                GOTO(out, rc = -EPROTO);
        }

        memcpy(osfs, msfs, sizeof (*msfs));
        EXIT;
out:
        ptlrpc_req_finished(req);
        return rc;
}

static int mdc_pin(struct obd_export *exp, obd_id ino, __u32 gen, int type,
                   struct obd_client_handle *handle, int flag)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[2] = {0, sizeof(*body)};
        ENTRY;

        //size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_PIN, 2, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        //mdc_pack_secdesc(req, size[0]);

        body = lustre_msg_buf(req->rq_reqmsg, 
                              MDS_REQ_REC_OFF, sizeof(*body));

        /* FIXME-UMKA: here should be also mdsnum and fid. */
        mdc_pack_id(&body->id1, ino, gen, type, 0, 0);
        body->flags = flag;

        req->rq_replen = lustre_msg_size(1, &size[1]);

        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        if (rc) {
                CERROR("pin failed: %d\n", rc);
                ptlrpc_req_finished(req);
                RETURN(rc);
        }

        body = lustre_swab_repbuf(req, 0, sizeof(*body), lustre_swab_mds_body);
        if (body == NULL) {
                ptlrpc_req_finished(req);
                RETURN(rc);
        }

        memcpy(&handle->och_fh, &body->handle, sizeof(body->handle));
        handle->och_magic = OBD_CLIENT_HANDLE_MAGIC;

        OBD_ALLOC(handle->och_mod, sizeof(*handle->och_mod));
        if (handle->och_mod == NULL) {
                DEBUG_REQ(D_ERROR, req, "can't allocate mdc_open_data");
                RETURN(-ENOMEM);
        }
        handle->och_mod->mod_open_req = req; /* will be dropped by unpin */

        RETURN(rc);
}

static int mdc_unpin(struct obd_export *exp,
                     struct obd_client_handle *handle, int flag)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[2] = {0, sizeof(*body)};
        ENTRY;

        if (handle->och_magic != OBD_CLIENT_HANDLE_MAGIC)
                RETURN(0);

        //size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_CLOSE, 2, size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        //mdc_pack_secdesc(req, size[0]);

        body = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF, sizeof(*body));
        memcpy(&body->handle, &handle->och_fh, sizeof(body->handle));
        body->flags = flag;

        req->rq_replen = lustre_msg_size(0, NULL);
        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);

        if (rc != 0)
                CERROR("unpin failed: %d\n", rc);

        ptlrpc_req_finished(req);
        ptlrpc_req_finished(handle->och_mod->mod_open_req);
        OBD_FREE(handle->och_mod, sizeof(*handle->och_mod));
        RETURN(rc);
}

int mdc_sync(struct obd_export *exp, struct lustre_id *id,
             struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        int size[2] = {0, sizeof(*body)};
        int rc;
        ENTRY;

        //size[0] = mdc_get_secdesc_size();

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_MDS_VERSION,
                              MDS_SYNC, 2, size, NULL);
        if (!req)
                RETURN(rc = -ENOMEM);

        //mdc_pack_secdesc(req, size[0]);

        if (id) {
                body = lustre_msg_buf(req->rq_reqmsg, MDS_REQ_REC_OFF,
                                      sizeof (*body));
                memcpy(&body->id1, id, sizeof(*id));
        }

        req->rq_replen = lustre_msg_size(1, &size[1]);

        rc = ptlrpc_queue_wait(req);
        if (rc || request == NULL)
                ptlrpc_req_finished(req);
        else
                *request = req;

        RETURN(rc);
}

static int mdc_import_event(struct obd_device *obd, struct obd_import *imp,
                            enum obd_import_event event)
{
        int rc = 0;

        LASSERT(imp->imp_obd == obd);

        switch (event) {
        case IMP_EVENT_DISCON: {
                break;
        }
        case IMP_EVENT_INACTIVE: {
                if (obd->obd_observer)
                        rc = obd_notify(obd->obd_observer, obd, 0, 0);
                break;
        }
        case IMP_EVENT_INVALIDATE: {
                struct ldlm_namespace *ns = obd->obd_namespace;

                ldlm_namespace_cleanup(ns, LDLM_FL_LOCAL_ONLY);

                break;
        }
        case IMP_EVENT_ACTIVE: {
                if (obd->obd_observer)
                        rc = obd_notify(obd->obd_observer, obd, 1, 0);
                break;
        }
        default:
                CERROR("Unknown import event %d\n", event);
                LBUG();
        }
        RETURN(rc);
}

static int mdc_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(mdc, &lvars);
        return lprocfs_obd_attach(dev, lvars.obd_vars);
}

static int mdc_detach(struct obd_device *dev)
{
        return lprocfs_obd_detach(dev);
}

static int mdc_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct client_obd *cli = &obd->u.cli;
        int rc;
        ENTRY;

        OBD_ALLOC(cli->cl_rpc_lock, sizeof (*cli->cl_rpc_lock));
        if (!cli->cl_rpc_lock)
                RETURN(-ENOMEM);
        mdc_init_rpc_lock(cli->cl_rpc_lock);

        ptlrpcd_addref();

        OBD_ALLOC(cli->cl_setattr_lock, sizeof (*cli->cl_setattr_lock));
        if (!cli->cl_setattr_lock)
                GOTO(err_rpc_lock, rc = -ENOMEM);
        mdc_init_rpc_lock(cli->cl_setattr_lock);

        rc = client_obd_setup(obd, len, buf);
        if (rc)
                GOTO(err_setattr_lock, rc);

        rc = obd_llog_init(obd, &obd->obd_llogs, obd, 0, NULL);
        if (rc) {
                mdc_cleanup(obd, 0);
                CERROR("failed to setup llogging subsystems\n");
        }

        RETURN(rc);

err_setattr_lock:
        OBD_FREE(cli->cl_setattr_lock, sizeof (*cli->cl_setattr_lock));
err_rpc_lock:
        OBD_FREE(cli->cl_rpc_lock, sizeof (*cli->cl_rpc_lock));
        ptlrpcd_decref();
        RETURN(rc);
}

static int mdc_init_ea_size(struct obd_export *exp, int easize, int cookiesize)
{
        struct obd_device *obd = exp->exp_obd;
        struct client_obd *cli = &obd->u.cli;
        ENTRY;

        if (cli->cl_max_mds_easize < easize)
                cli->cl_max_mds_easize = easize;
        if (cli->cl_max_mds_cookiesize < cookiesize)
                cli->cl_max_mds_cookiesize = cookiesize;
        RETURN(0);
}

static int mdc_precleanup(struct obd_device *obd, int flags)
{
        int rc = 0;
        
        rc = obd_llog_finish(obd, &obd->obd_llogs, 0);
        if (rc != 0)
                CERROR("failed to cleanup llogging subsystems\n");

        RETURN(rc);
}

static int mdc_cleanup(struct obd_device *obd, int flags)
{
        struct client_obd *cli = &obd->u.cli;

        OBD_FREE(cli->cl_rpc_lock, sizeof (*cli->cl_rpc_lock));
        OBD_FREE(cli->cl_setattr_lock, sizeof (*cli->cl_setattr_lock));

        ptlrpcd_decref();

        return client_obd_cleanup(obd, flags);
}


static int mdc_llog_init(struct obd_device *obd, struct obd_llogs *llogs, 
                         struct obd_device *tgt, int count,
                         struct llog_catid *logid)
{
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        rc = obd_llog_setup(obd, llogs, LLOG_CONFIG_REPL_CTXT, tgt, 0, NULL,
                            &llog_client_ops);
        if (rc == 0) {
                ctxt = llog_get_context(llogs, LLOG_CONFIG_REPL_CTXT);
                ctxt->loc_imp = obd->u.cli.cl_import;
        }

        RETURN(rc);
}

static int mdc_llog_finish(struct obd_device *obd,
                           struct obd_llogs *llogs, int count)
{
        int rc;
        ENTRY;

        rc = obd_llog_cleanup(llog_get_context(llogs, LLOG_CONFIG_REPL_CTXT));
        RETURN(rc);
}
static struct obd_device *mdc_get_real_obd(struct obd_export *exp,
                                           struct lustre_id *id)
{
       ENTRY;
       RETURN(exp->exp_obd);
}

static int mdc_get_info(struct obd_export *exp, obd_count keylen,
                        void *key, __u32 *valsize, void *val)
{
        struct ptlrpc_request *req;
        char *bufs[1] = {key};
        int rc = 0;
        ENTRY;
        
        if (!valsize || !val)
                RETURN(-EFAULT);

        if ((keylen < strlen("mdsize") || strcmp(key, "mdsize") != 0) &&
            (keylen < strlen("mdsnum") || strcmp(key, "mdsnum") != 0) &&
            (keylen < strlen("rootid") || strcmp(key, "rootid") != 0))
                RETURN(-EPROTO);
                
        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_OBD_VERSION,
                              OST_GET_INFO, 1, &keylen, bufs);
        if (req == NULL)
                RETURN(-ENOMEM);

        req->rq_replen = lustre_msg_size(1, valsize);
        rc = ptlrpc_queue_wait(req);
        if (rc)
                GOTO(out_req, rc);

        if (keylen >= strlen("rootid") && !strcmp(key, "rootid")) {
                struct lustre_id *reply;
                
                reply = lustre_swab_repbuf(req, 0, sizeof(*reply),
                                           lustre_swab_lustre_id);
                if (reply == NULL) {
                        CERROR("Can't unpack %s\n", (char *)key);
                        GOTO(out_req, rc = -EPROTO);
                }

                *(struct lustre_id *)val = *reply;
        } else {
                __u32 *reply;
                
                reply = lustre_swab_repbuf(req, 0, sizeof(*reply),
                                           lustre_swab_generic_32s);
                if (reply == NULL) {
                        CERROR("Can't unpack %s\n", (char *)key);
                        GOTO(out_req, rc = -EPROTO);
                }
                *((__u32 *)val) = *reply;
        }
out_req:
        ptlrpc_req_finished(req);
        RETURN(rc);
}

int mdc_obj_create(struct obd_export *exp, struct obdo *oa,
                    struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        LASSERT(oa);

        request = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_OBD_VERSION,
                                  OST_CREATE, 1, &size, NULL);
        if (!request)
                GOTO(out_req, rc = -ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0, sizeof (*body));
        memcpy(&body->oa, oa, sizeof(body->oa));

        request->rq_replen = lustre_msg_size(1, &size);
        rc = ptlrpc_queue_wait(request);
        if (rc)
                GOTO(out_req, rc);

        body = lustre_swab_repbuf(request, 0, sizeof(*body),
                                  lustre_swab_ost_body);
        if (body == NULL) {
                CERROR ("can't unpack ost_body\n");
                GOTO (out_req, rc = -EPROTO);
        }

        memcpy(oa, &body->oa, sizeof(*oa));

        /* store ino/generation for recovery */
        body = lustre_msg_buf(request->rq_reqmsg, 0, sizeof (*body));
        body->oa.o_id = oa->o_id;
        body->oa.o_generation = oa->o_generation;
        body->oa.o_fid = oa->o_fid;
        body->oa.o_mds = oa->o_mds;

        CDEBUG(D_HA, "transno: "LPD64"\n", request->rq_repmsg->transno);
        EXIT;
out_req:
        ptlrpc_req_finished(request);
        return rc;
}

int mdc_brw(int rw, struct obd_export *exp, struct obdo *oa,
            struct lov_stripe_md *ea, obd_count oa_bufs,
            struct brw_page *pgarr, struct obd_trans_info *oti)
{
        struct ptlrpc_bulk_desc *desc;
        struct niobuf_remote *niobuf;
        struct ptlrpc_request *req;
        struct obd_ioobj *ioobj;
        struct ost_body *body;
        int err, opc, i;
        int size[3];

        opc = ((rw & OBD_BRW_WRITE) != 0) ? OST_WRITE : OST_READ;
        
        size[0] = sizeof(*body);
        size[1] = sizeof(*ioobj);
        size[2] = oa_bufs * sizeof(*niobuf);

        req = ptlrpc_prep_req(class_exp2cliimp(exp), LUSTRE_OBD_VERSION, opc,
                              3, size, NULL);
        LASSERT(req != NULL);

        if (opc == OST_WRITE)
                desc = ptlrpc_prep_bulk_imp(req, oa_bufs, BULK_GET_SOURCE,
                                            OST_BULK_PORTAL);
        else
                desc = ptlrpc_prep_bulk_imp(req, oa_bufs, BULK_PUT_SINK,
                                            OST_BULK_PORTAL);
        LASSERT(desc != NULL);

        body = lustre_msg_buf(req->rq_reqmsg, 0, sizeof(*body));
        ioobj = lustre_msg_buf(req->rq_reqmsg, 1, sizeof(*ioobj));
        niobuf = lustre_msg_buf(req->rq_reqmsg, 2, oa_bufs * sizeof(*niobuf));

        memcpy(&body->oa, oa, sizeof(*oa));
        obdo_to_ioobj(oa, ioobj);
        ioobj->ioo_bufcnt = oa_bufs;

        for (i = 0; i < oa_bufs; i++, niobuf++) {
                struct brw_page *pg = &pgarr[i];

                LASSERT(pg->count > 0);
                LASSERT((pg->disk_offset & ~PAGE_MASK) + pg->count <= PAGE_SIZE);

                ptlrpc_prep_bulk_page(desc, pg->pg, pg->disk_offset & ~PAGE_MASK,
                                      pg->count);

                niobuf->offset = pg->disk_offset;
                niobuf->len = pg->count;
                niobuf->flags = pg->flag;
        }

        /* size[0] still sizeof (*body) */
        if (opc == OST_WRITE) {
                /* 1 RC per niobuf */
                size[1] = sizeof(__u32) * oa_bufs;
                req->rq_replen = lustre_msg_size(2, size);
        } else {
                /* 1 RC for the whole I/O */
                req->rq_replen = lustre_msg_size(1, size);
        }
        err = ptlrpc_queue_wait(req);
        LASSERT(err == 0);

        ptlrpc_req_finished(req);
        return 0;
}

static int mdc_valid_attrs(struct obd_export *exp,
                           struct lustre_id *id)
{
        struct ldlm_res_id res_id = { .name = {0} };
        struct obd_device *obd = exp->exp_obd;
        struct lustre_handle lockh;
        ldlm_policy_data_t policy;
        int flags;
        ENTRY;

        res_id.name[0] = id_fid(id);
        res_id.name[1] = id_group(id);
        policy.l_inodebits.bits = MDS_INODELOCK_UPDATE;

        CDEBUG(D_INFO, "trying to match res "LPU64"\n",
               res_id.name[0]);

        /* FIXME use LDLM_FL_TEST_LOCK instead */
        flags = LDLM_FL_BLOCK_GRANTED | LDLM_FL_CBPENDING;
        if (ldlm_lock_match(obd->obd_namespace, flags, &res_id,
                            LDLM_IBITS, &policy, LCK_PR, &lockh)) {
                ldlm_lock_decref(&lockh, LCK_PR);
                RETURN(1);
        }

        if (ldlm_lock_match(obd->obd_namespace, flags, &res_id,
                            LDLM_IBITS, &policy, LCK_PW, &lockh)) {
                ldlm_lock_decref(&lockh, LCK_PW);
                RETURN(1);
        }
        RETURN(0);
}

static int mdc_change_cbdata_name(struct obd_export *exp,
                                  struct lustre_id *pid,
                                  char *name, int len,
                                  struct lustre_id *cid,
                                  ldlm_iterator_t it, void *data)
{
        int rc;
        rc = mdc_change_cbdata(exp, cid, it, data);
        RETURN(rc);
}

struct obd_ops mdc_obd_ops = {
        .o_owner         = THIS_MODULE,
        .o_attach        = mdc_attach,
        .o_detach        = mdc_detach,
        .o_setup         = mdc_setup,
        .o_precleanup    = mdc_precleanup,
        .o_cleanup       = mdc_cleanup,
        .o_add_conn      = client_import_add_conn,
        .o_del_conn      = client_import_del_conn,
        .o_connect       = client_connect_import,
        .o_disconnect    = client_disconnect_export,
        .o_iocontrol     = mdc_iocontrol,
        .o_statfs        = mdc_statfs,
        .o_pin           = mdc_pin,
        .o_unpin         = mdc_unpin,
        .o_import_event  = mdc_import_event,
        .o_llog_init     = mdc_llog_init,
        .o_llog_finish   = mdc_llog_finish,
        .o_create        = mdc_obj_create,
        .o_set_info      = mdc_set_info,
        .o_get_info      = mdc_get_info,
        .o_brw           = mdc_brw,
        .o_init_ea_size  = mdc_init_ea_size,
};

struct md_ops mdc_md_ops = {
        .m_getstatus     = mdc_getstatus,
        .m_getattr       = mdc_getattr,
        .m_close         = mdc_close,
        .m_create        = mdc_create,
        .m_done_writing  = mdc_done_writing,
        .m_enqueue       = mdc_enqueue,
        .m_getattr_lock  = mdc_getattr_lock,
        .m_intent_lock   = mdc_intent_lock,
        .m_link          = mdc_link,
        .m_rename        = mdc_rename,
        .m_setattr       = mdc_setattr,
        .m_sync          = mdc_sync,
        .m_readpage      = mdc_readpage,
        .m_unlink        = mdc_unlink,
        .m_valid_attrs   = mdc_valid_attrs,
        .m_req2lustre_md = mdc_req2lustre_md,
        .m_set_open_replay_data   = mdc_set_open_replay_data,
        .m_clear_open_replay_data = mdc_clear_open_replay_data,
        .m_store_inode_generation = mdc_store_inode_generation,
        .m_set_lock_data = mdc_set_lock_data,
        .m_get_real_obd  = mdc_get_real_obd,
        .m_change_cbdata_name = mdc_change_cbdata_name,
        .m_change_cbdata = mdc_change_cbdata,
};

int __init mdc_init(void)
{
        struct lprocfs_static_vars lvars;
        lprocfs_init_vars(mdc, &lvars);
        return class_register_type(&mdc_obd_ops, &mdc_md_ops, lvars.module_vars,
                                   LUSTRE_MDC_NAME);
}

#ifdef __KERNEL__
static void /*__exit*/ mdc_exit(void)
{
        class_unregister_type(LUSTRE_MDC_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Metadata Client");
MODULE_LICENSE("GPL");

EXPORT_SYMBOL(mdc_req2lustre_md);
EXPORT_SYMBOL(mdc_change_cbdata);
EXPORT_SYMBOL(mdc_getstatus);
EXPORT_SYMBOL(mdc_getattr);
EXPORT_SYMBOL(mdc_getattr_lock);
EXPORT_SYMBOL(mdc_create);
EXPORT_SYMBOL(mdc_unlink);
EXPORT_SYMBOL(mdc_rename);
EXPORT_SYMBOL(mdc_link);
EXPORT_SYMBOL(mdc_readpage);
EXPORT_SYMBOL(mdc_setattr);
EXPORT_SYMBOL(mdc_close);
EXPORT_SYMBOL(mdc_done_writing);
EXPORT_SYMBOL(mdc_sync);
EXPORT_SYMBOL(mdc_set_open_replay_data);
EXPORT_SYMBOL(mdc_clear_open_replay_data);
EXPORT_SYMBOL(mdc_store_inode_generation);

module_init(mdc_init);
module_exit(mdc_exit);
#endif
