/*
linphone
Copyright (C) 2012  Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#include "sal_impl.h"


/*create an operation */
SalOp * sal_op_new(Sal *sal){
	SalOp *op=ms_new0(SalOp,1);
	__sal_op_init(op,sal);
	return op;
}

void sal_op_release(SalOp *op){
	if (op->request) belle_sip_object_unref(op->request);
	if (op->registration_refresh_timer>0) {
		belle_sip_main_loop_cancel_source(belle_sip_stack_get_main_loop(op->base.root->stack),op->registration_refresh_timer);
	}
	__sal_op_free(op);
	return ;
}
void sal_op_authenticate(SalOp *op, const SalAuthInfo *info){
	/*for sure auth info will be accesible from the provider*/
	sal_process_authentication(op, NULL);
	return ;
}

void sal_op_cancel_authentication(SalOp *h){
	ms_fatal("sal_op_cancel_authentication not implemented yet");
	return ;
}

int sal_op_get_auth_requested(SalOp *op, const char **realm, const char **username){
	*realm=op->auth_info.realm;
	*username=op->auth_info.username;
	return 0;
}

belle_sip_request_t* sal_op_build_request(SalOp *op,const char* method) {
	belle_sip_header_from_t* from_header;
	belle_sip_header_to_t* to_header;
	belle_sip_provider_t* prov=op->base.root->prov;
	belle_sip_request_t *req;
	belle_sip_uri_t* req_uri;
	belle_sip_header_contact_t* contact_header;
	char token[10];

	from_header = belle_sip_header_from_create(BELLE_SIP_HEADER_ADDRESS(sal_op_get_from_address(op))
												,belle_sip_random_token(token,sizeof(token)));
	to_header = belle_sip_header_to_create(BELLE_SIP_HEADER_ADDRESS(sal_op_get_to_address(op)),NULL);
	req_uri = (belle_sip_uri_t*)belle_sip_object_clone((belle_sip_object_t*)belle_sip_header_address_get_uri((belle_sip_header_address_t*)to_header));

	if (sal_op_get_contact_address(op)) {
		contact_header = belle_sip_header_contact_create(BELLE_SIP_HEADER_ADDRESS(sal_op_get_contact_address(op)));
	} else {
		contact_header= belle_sip_header_contact_new();
		belle_sip_header_address_set_uri((belle_sip_header_address_t*)contact_header,belle_sip_uri_new());
		belle_sip_uri_set_user(belle_sip_header_address_get_uri((belle_sip_header_address_t*)contact_header),belle_sip_uri_get_user(req_uri));
	}
	req=belle_sip_request_create(
							req_uri,
							method,
		                    belle_sip_provider_create_call_id(prov),
		                    belle_sip_header_cseq_create(20,method),
		                    from_header,
		                    to_header,
		                    belle_sip_header_via_new(),
		                    70);
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(contact_header));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(op->base.root->user_agent));
	return req;
}



/*ping: main purpose is to obtain its own contact address behind firewalls*/
int sal_ping(SalOp *op, const char *from, const char *to){
	sal_op_set_from(op,from);
	sal_op_set_to(op,to);
	return sal_op_send_request(op,sal_op_build_request(op,"OPTION"));
}

void sal_op_set_remote_ua(SalOp*op,belle_sip_message_t* message) {
	belle_sip_header_user_agent_t* user_agent=belle_sip_message_get_header_by_type(message,belle_sip_header_user_agent_t);
	char user_agent_string[256];
	if(user_agent && belle_sip_header_user_agent_get_products_as_string(user_agent,user_agent_string,sizeof(user_agent_string))>0) {
		op->base.remote_ua=ms_strdup(user_agent_string);
	}
}

void sal_op_resend_request(SalOp* op, belle_sip_request_t* request) {
	belle_sip_header_cseq_t* cseq=(belle_sip_header_cseq_t*)belle_sip_message_get_header(BELLE_SIP_MESSAGE(op->request),BELLE_SIP_CSEQ);
	belle_sip_header_cseq_set_seq_number(cseq,belle_sip_header_cseq_get_seq_number(cseq)+1);
	sal_op_send_request(op,request);
}

int sal_op_send_request(SalOp* op, belle_sip_request_t* request) {
	belle_sip_client_transaction_t* client_transaction;
	belle_sip_provider_t* prov=op->base.root->prov;
	belle_sip_header_route_t* route_header;
	if (!op->dialog) {
		/*don't put route header if dialog is in confirmed state*/
		if (sal_op_get_route_address(op)) {
			route_header = belle_sip_header_route_create(BELLE_SIP_HEADER_ADDRESS(sal_op_get_route_address(op)));
			belle_sip_message_add_header(BELLE_SIP_MESSAGE(request),BELLE_SIP_HEADER(route_header));
		}
	}

	client_transaction = belle_sip_provider_create_client_transaction(prov,request);
	belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(client_transaction),op);
	if ( strcmp("INVITE",belle_sip_request_get_method(request))==0) {
		op->pending_inv_client_trans=client_transaction; /*update pending inv for being able to cancel*/
	}
	
	if (!belle_sip_message_get_header(BELLE_SIP_MESSAGE(request),BELLE_SIP_AUTHORIZATION)
		&& !belle_sip_message_get_header(BELLE_SIP_MESSAGE(request),BELLE_SIP_PROXY_AUTHORIZATION)) {
		/*hmm just in case we already have authentication param in cache*/
		belle_sip_provider_add_authorization(op->base.root->prov,request,NULL);
	}
	return belle_sip_client_transaction_send_request(client_transaction);

}
/*return TRUE if error code*/
bool_t sal_compute_sal_errors(belle_sip_response_t* response,SalError* sal_err,SalReason* sal_reason,char* reason, size_t reason_size) {
	int code = belle_sip_response_get_status_code(response);
	belle_sip_header_t* reason_header = belle_sip_message_get_header(BELLE_SIP_MESSAGE(response),"Reason");
	*sal_err=SalErrorUnknown;
	*sal_reason = SalReasonUnknown;

	if (reason_header){
		snprintf(reason
				,reason_size
				,"%s %s"
				,belle_sip_response_get_reason_phrase(response)
				,belle_sip_header_extension_get_value(BELLE_SIP_HEADER_EXTENSION(reason_header)));
	} else {
		strncpy(reason,belle_sip_response_get_reason_phrase(response),reason_size);
	}
	if (code >=400) {
		switch(code) {
		case 400:
			*sal_err=SalErrorUnknown;
			break;
		case 404:
			*sal_err=SalErrorFailure;
			*sal_reason=SalReasonNotFound;
			break;
		case 415:
			*sal_err=SalErrorFailure;
			*sal_reason=SalReasonMedia;
			break;
		case 422:
			ms_error ("422 not implemented yet");;
			break;
		case 480:
			*sal_err=SalErrorFailure;
			*sal_reason=SalReasonTemporarilyUnavailable;
			break;
		case 486:
			*sal_err=SalErrorFailure;
			*sal_reason=SalReasonBusy;
			break;
		case 487:
			break;
		case 600:
			*sal_err=SalErrorFailure;
			*sal_reason=SalReasonDoNotDisturb;
			break;
		case 603:
			*sal_err=SalErrorFailure;
			*sal_reason=SalReasonDeclined;
			break;
		default:
			if (code>0){
				*sal_err=SalErrorFailure;
				*sal_reason=SalReasonUnknown;
			}else *sal_err=SalErrorNoResponse;
			/* no break */
		}
		return TRUE;
	} else {
		return FALSE;
	}
}
void set_or_update_dialog(SalOp* op, const belle_sip_response_event_t* event) {
	/*check if dialog has changed*/
	if (belle_sip_response_event_get_dialog(event) != op->dialog) {
		ms_message("Dialog set from [%p] to [%p] for op [%p]",op->dialog,belle_sip_response_event_get_dialog(event),op);
		/*fixme, shouldn't we cancel previous dialog*/
		if (op->dialog)belle_sip_object_unref(op->dialog);
		op->dialog=belle_sip_response_event_get_dialog(event);
		belle_sip_dialog_set_application_data(op->dialog,op);
		belle_sip_object_ref(op->dialog);
	}
}
