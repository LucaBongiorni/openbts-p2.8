/*
* Copyright 2008, 2009, 2010m 2011 Free Software Foundation, Inc.
* Copyright 2011 Range Networks, Inc.
*
* This software is distributed under the terms of the GNU Affero Public License.
* See the COPYING file in the main directory for details.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/




#include <ortp/ortp.h>
#include <osipparser2/sdp_message.h>

#include <GSMLogicalChannel.h>
#include <GSMConfig.h>
#include <ControlCommon.h>
#include <TransactionTable.h>
#include <SubscriberRegistry.h>

#include <Sockets.h>

#include "SIPUtility.h"
#include "SIPInterface.h"

#include <Logger.h>

#undef WARNING



using namespace std;
using namespace SIP;

using namespace GSM;
using namespace Control;




// SIPMessageMap method definitions.

void SIPMessageMap::write(const std::string& call_id, osip_message_t * msg)
{
	LOG(DEBUG) << "call_id=" << call_id << " msg=" << msg;
	string name = osip_message_get_from(msg)->url->username;
	if (!gSubscriberRegistry.imsiSet(name, "ipaddr", 
					 osip_message_get_from(msg)->url->host)){
		LOG(INFO) << "SR ipaddr Update Problem";
	}
	if (!gSubscriberRegistry.imsiSet(name, "port", 
					 gConfig.getStr("SIP.Local.Port"))){
		LOG(INFO) << "SR port Update Problem";
	}
	OSIPMessageFIFO * fifo = mMap.readNoBlock(call_id);
	if(NULL == fifo) {
		// FIXME -- If this write fails, send "call leg non-existent" response on SIP interface.
		LOG(NOTICE) << "missing SIP FIFO " << call_id;
		throw SIPError();
	}
	LOG(DEBUG) << "write on fifo " << fifo;
	fifo->write(msg);	
}

osip_message_t * SIPMessageMap::read(const std::string& call_id, unsigned readTimeout)
{ 
	LOG(DEBUG) << "call_id=" << call_id;
	OSIPMessageFIFO * fifo = mMap.readNoBlock(call_id);
	if (!fifo) {
		LOG(NOTICE) << "missing SIP FIFO "<< call_id;
		throw SIPError();
	}
	LOG(DEBUG) << "blocking on fifo " << fifo << " for " << readTimeout << " msec." << endl;
	osip_message_t * msg =  fifo->read(readTimeout);	
	if (!msg) {
	  LOG(DEBUG) << "call_id=" << call_id << "no message: throwing SIPTimeout";
	  throw SIPTimeout();
	}
	LOG(DEBUG) << "call_id=" << call_id << " msg=" << msg;
	return msg;
}


bool SIPMessageMap::add(const std::string& call_id, const struct sockaddr_in* returnAddress)
{
	OSIPMessageFIFO * fifo = new OSIPMessageFIFO(returnAddress);
	mMap.write(call_id, fifo);
	return true;
}

bool SIPMessageMap::remove(const std::string& call_id)
{
	OSIPMessageFIFO * fifo = mMap.readNoBlock(call_id);
	if(fifo == NULL) return false;
	mMap.remove(call_id);
	return true;
}





// SIPInterface method definitions.

bool SIPInterface::addCall(const string &call_id)
{
	LOG(INFO) << "creating SIP message FIFO callID " << call_id;
	return mSIPMap.add(call_id,mSIPSocket.source());
}


bool SIPInterface::removeCall(const string &call_id)
{
	LOG(INFO) << "removing SIP message FIFO callID " << call_id;
	return mSIPMap.remove(call_id);
}

int SIPInterface::fifoSize(const std::string& call_id )
{ 
	OSIPMessageFIFO * fifo = mSIPMap.map().read(call_id,0);
	if(fifo==NULL) return -1;
	return fifo->size();
}	





void SIP::driveLoop( SIPInterface * si){
	while (true) {
		si->drive();
	}
}

void SIPInterface::start(){
	// Start all the osip/ortp stuff. 
	parser_init();
	ortp_init();
	ortp_scheduler_init();
	// FIXME -- Can we coordinate this with the global logger?
	//ortp_set_log_level_mask(ORTP_MESSAGE|ORTP_WARNING|ORTP_ERROR);
	mDriveThread.start((void *(*)(void*))driveLoop,this );
}




void SIPInterface::write(const struct sockaddr_in* dest, osip_message_t *msg) 
{
	char * str;
	size_t msgSize;
	osip_message_to_str(msg, &str, &msgSize);
	if (!str) {
		LOG(ERR) << "osip_message_to_str produced a NULL pointer.";
		return;
	}
	char firstLine[100];
	sscanf(str,"%100[^\n]",firstLine);
	LOG(INFO) << "write " << firstLine;
	LOG(DEBUG) << "write " << str;

	mSocketLock.lock();
	mSIPSocket.send((const struct sockaddr*)dest,str);
	mSocketLock.unlock();
	free(str);
}




void SIPInterface::drive() 
{// All inbound SIP messages go here for processing.

	LOG(DEBUG) << "blocking on socket";
	int numRead = mSIPSocket.read(mReadBuffer);
	if (numRead < 0) {
	    LOG(ALERT) << "cannot read SIP socket.";
	    return;
	} else {
	    LOG(DEBUG) << numRead << " bytes read";
	}
	// FIXME -- Is this +1 offset correct?  Check it.
	mReadBuffer[numRead] = '\0';

	// Get the proxy from the inbound message.
#if 0
	const struct sockaddr_in* sourceAddr = mSIPSocket.source();
	char msgHost[256];
	const char* msgHostRet = inet_ntop(AF_INET,&(sourceAddr->sin_addr),msgHost,255);
	if (!msgHostRet) {
		LOG(ALERT) << "cannot translate SIP source address for " << mReadBuffer;
		return;
	}
	unsigned msgPortNumber = sourceAddr->sin_port;
	char msgPort[20];
	sprintf(msgPort,"%u",msgPortNumber);
	string proxy = string(msgHost) + string(":") + string(msgPort);
#endif

	char firstLine[101];
	sscanf(mReadBuffer,"%100[^\n]", firstLine);
	LOG(INFO) << "read " << firstLine;
	LOG(DEBUG) << "read " << mReadBuffer;

	try {// Parse the mesage.
		osip_message_t * msg;
		int osip = osip_message_init(&msg);
		if (0 != osip) {
		    LOG(ERR) << "SIP failure - osip_message_init returned: " << osip;
    		    throw SIPError();
		}
		osip = osip_message_parse(msg, mReadBuffer, numRead);
		if (0 != osip) {
		    LOG(ERR) << "SIP failure - osip_message_parse returned: " << osip;
    		    throw SIPError();
		}
		
		char * dest = NULL;
		size_t length = 0;
		char *p = strstr(mReadBuffer, "nonce");
		if (p) {
		  LOG(DEBUG) << "workaround for oSIP parser revealed: " << strndup(p + 6, 32) << endl;
		  osip_www_authenticate_t *auth;
		  osip_www_authenticate_init(&auth);
		  osip_www_authenticate_set_auth_type(auth, osip_strdup("Digest"));
		  osip_www_authenticate_set_nonce(auth, osip_strdup(strndup(p + 6, 32)));
		  osip = osip_list_add (&msg->www_authenticates, auth, -1);
		  if (osip < 0) LOG(ERR) << "problem adding www_authenticate" << endl;
		}

		if (msg->sip_method) LOG(DEBUG) << "read method " << msg->sip_method;
   
		osip = osip_message_to_str(msg, &dest, &length);
		if (0 != osip) {
		    LOG(INFO) << "cannot get printable message";
		} else {
		    LOG(INFO) << "parsed SIP: " << dest << endl;
		    osip_free(dest);
		}

		// Must check if msg is an invite.
		// if it is, handle appropriatly.
		// FIXME -- Check return value in case this failed.
		// FIXME -- If we support USSD via SIP, we will need to check the map first.
		checkInvite(msg);

		// Multiplex out the received SIP message to active calls.

		// If we write to non-existent call_id.
		// this is errant message so need to catch
		// Internal error excatpion. and give nice
		// message (instead of aborting)
		// Don't free call_id_num.  It points into msg->call_id.
		char * call_id_num = osip_call_id_get_number(msg->call_id);	
		if(NULL == call_id_num) {
			LOG(WARNING) << "message with no call id";
			throw SIPError();
		}
		LOG(DEBUG) << "got message with call id " << call_id_num << " and writing it to the map.";
		string call_num(call_id_num);
		// Don't free msg.  Whoever reads the FIFO will do that.
		mSIPMap.write(call_num, msg);
	}
	catch(SIPException) {
		LOG(WARNING) << "failed to handle SIP message: " << mReadBuffer;
	}
}




const char* extractIMSI(const osip_message_t *msg)
{
	// Get request username (IMSI) from invite. 
	// Form of the name is IMSI<digits>, and it should always be 18 or 19 char.
	const char * IMSI = msg->req_uri->username;
	LOG(INFO) << msg->sip_method << " to "<< IMSI;
	// IMSIs are 14 or 15 char + "IMSI" prefix
	// FIXME -- We need better validity-checking.
	unsigned namelen = strlen(IMSI);
	if ((namelen>19)||(namelen<18)) {
		LOG(WARNING) << "INVITE with malformed username \"" << IMSI << "\"";
		return false;
	}
	// Skip first 4 char "IMSI".
	return IMSI+4;
}


const char* extractCallID(const osip_message_t* msg)
{
	if (!msg->call_id) return NULL;
	return  osip_call_id_get_number(msg->call_id);	

}




bool SIPInterface::checkInvite( osip_message_t * msg)
{
	LOG(DEBUG);

	// This code dispatches new transactions coming from the network-side SIP interface.
	// All transactions originating here are going to be mobile-terminated.
	// Yes, this method is too long and needs to be broken up into smaller steps.

	// Is there even a method?
	const char *method = msg->sip_method;
	if (!method) return false;

	// Check for INVITE or MESSAGE methods.
	// Check channel availability now, too.
	GSM::ChannelType requiredChannel;
	bool channelAvailable = false;
	GSM::L3CMServiceType serviceType;
	// pretty sure strings are garbage collected
	string proxy = get_return_address(msg);
	if (strcmp(method,"INVITE") == 0) {
		// INVITE is for MTC.
		// Set the required channel type to match the assignment style.
		if (gConfig.defines("Control.VEA")) {
			// Very early assignment.
			requiredChannel = GSM::TCHFType;
			channelAvailable = gBTS.TCHAvailable();
		} else {
			// Early assignment
			requiredChannel = GSM::SDCCHType;
			channelAvailable = gBTS.SDCCHAvailable() && gBTS.TCHAvailable();
		}
		serviceType = L3CMServiceType::MobileTerminatedCall;
	}
	else if (strcmp(method,"MESSAGE") == 0) {
		// MESSAGE is for MTSMS.
		requiredChannel = GSM::SDCCHType;
		channelAvailable = gBTS.SDCCHAvailable();
		serviceType = L3CMServiceType::MobileTerminatedShortMessage;
	}
	else {
		// Not a method handled here.
		LOG(DEBUG) << "non-initiating SIP method " << method;
		return false;
	}

	// Get request username (IMSI) from invite. 
	const char* IMSI = extractIMSI(msg);
	if (!IMSI) {
		// FIXME -- Send appropriate error (404) on SIP interface.
		LOG(WARNING) << "Incoming INVITE/MESSAGE with no IMSI";
		return false;
	}
	L3MobileIdentity mobileID(IMSI);

	// Get the SIP call ID.
	const char * callIDNum = extractCallID(msg);	
	if (!callIDNum) {
		// FIXME -- Send appropriate error on SIP interface.
		LOG(WARNING) << "Incoming INVITE/MESSAGE with no call ID";
		return false;
	}


	// Find any active transaction for this IMSI with an assigned TCH or SDCCH.
	GSM::LogicalChannel *chan = gTransactionTable.findChannel(mobileID);
	if (chan) {
		// If the type is TCH and the service is SMS, get the SACCH.
		// Otherwise, for now, just say chan=NULL.
		if (serviceType==L3CMServiceType::MobileTerminatedShortMessage && chan->type()==FACCHType) {
			chan = chan->SACCH();
		} else {
			// FIXME -- This will change to support multiple transactions.
			chan = NULL;
		}
	}

	// Check SIP map.  Repeated entry?  Page again.
	if (mSIPMap.map().readNoBlock(callIDNum) != NULL) { 
		TransactionEntry* transaction= gTransactionTable.find(mobileID,callIDNum);
		// There's a FIFO but no trasnaction record?
		if (!transaction) {
			LOG(WARNING) << "repeated INVITE/MESSAGE with no transaction record";
			// Delete the bogus FIFO.
			mSIPMap.remove(callIDNum);
			return false;
		}
		// There is transaction already.  Send trying.
		transaction->MTCSendTrying();
		// And if no channel is established yet, page again.
		if (!chan) {
			LOG(INFO) << "repeated SIP INVITE/MESSAGE, repaging for transaction " << *transaction; 
			gBTS.pager().addID(mobileID,requiredChannel,*transaction);
		}
		return false;
	}

	// So we will need a new channel.
	// Check gBTS for channel availability.
	if (!chan && !channelAvailable) {
		// FIXME -- Send 503 "Service Unavailable" response on SIP interface.
		// Don't forget the retry-after header.
		LOG(NOTICE) << "MTC CONGESTION, no " << requiredChannel << " availble for assignment";
		return false;
	}
	if (chan)  { LOG(INFO) << "using existing channel " << chan->descriptiveString(); }
	else { LOG(INFO) << "set up MTC paging for channel=" << requiredChannel; }


	// Add an entry to the SIP Map to route inbound SIP messages.
	addCall(callIDNum);
	LOG(DEBUG) << "callIDNum " << callIDNum << " IMSI " << IMSI;

	// Get the caller ID if it's available.
	const char *callerID = "";
	const char *callerHost = "";
	osip_from_t *from = osip_message_get_from(msg);
	if (from) {
		osip_uri_t* url = osip_contact_get_url(from);
		if (url) {
			if (url->username) callerID = url->username;
			if (url->host) callerHost = url->host;
		}
	} else {
		LOG(NOTICE) << "INVITE with no From: username for " << mobileID;
	}
	LOG(DEBUG) << "callerID " << callerID << "@" << callerHost;


	// Build the transaction table entry.
	// This constructor sets TI automatically for an MT transaction.
	TransactionEntry *transaction = new TransactionEntry(proxy.c_str(),mobileID,chan,serviceType,callerID);
	// FIXME -- These parameters should be arguments to the constructor above.
	transaction->SIPUser(callIDNum,IMSI,callerID,callerHost);
	transaction->saveINVITE(msg,false);
	// Tell the sender we are trying.
	transaction->MTCSendTrying();

	// SMS?  Get the text message body to deliver.
	if (serviceType == L3CMServiceType::MobileTerminatedShortMessage) {
		osip_body_t *body;
		osip_content_type_t *contentType;
		osip_message_get_body(msg,0,&body);
		contentType = osip_message_get_content_type(msg);
		const char *text = NULL;
		char *type = NULL;
		if (body) text = body->body;
		if (text) transaction->message(text, body->length);
		else LOG(NOTICE) << "MTSMS incoming MESSAGE method with no message body for " << mobileID;
		/* Ok, so osip does some funny stuff here. The MIME type is split into type and subType.
			Basically, text/plain becomes type=text, subType=plain. We need to put those together...
		*/
		if (contentType) {
			type = (char *)malloc(strlen(contentType->type)+strlen(contentType->subtype)+2);
		}
		if (type) {
			strcpy(type,contentType->type);
			strcat(type,"/");
			strcat(type,contentType->subtype);
			transaction->messageType(type);
			free(type);
		}
		else LOG(NOTICE) << "MTSMS incoming MESSAGE method with no content type (or memory error) for " << mobileID;
	}

	LOG(INFO) << "MTC MTSMS make transaction and add to transaction table: "<< *transaction;
	gTransactionTable.add(transaction); 

	// If there's an existing channel, skip the paging step.
	if (!chan) {
		// Add to paging list.
		LOG(DEBUG) << "MTC MTSMS new SIP invite, initial paging for mobile ID " << mobileID;
		gBTS.pager().addID(mobileID,requiredChannel,*transaction);	
	} else {
		// Add a transaction to an existing channel.
		chan->addTransaction(transaction);
		// FIXME -- We need to write something into the channel to trigger the new transaction.
		// We need to send a message into the chan's dispatch loop,
		// becasue we can't block this thread to run the transaction.
	}

	return true;
}





// vim: ts=4 sw=4
