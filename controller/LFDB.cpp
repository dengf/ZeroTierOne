/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2019  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

#include "LFDB.hpp"

#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>

#include "../osdep/OSUtils.hpp"
#include "../ext/cpp-httplib/httplib.h"

namespace ZeroTier
{

LFDB::LFDB(EmbeddedNetworkController *const nc,const Identity &myId,const char *path,const char *lfOwnerPrivate,const char *lfOwnerPublic,const char *lfNodeHost,int lfNodePort,bool storeOnlineState) :
	DB(nc,myId,path),
	_nc(nc),
	_myId(myId),
	_lfOwnerPrivate((lfOwnerPrivate) ? lfOwnerPrivate : ""),
	_lfOwnerPublic((lfOwnerPublic) ? lfOwnerPublic : ""),
	_lfNodeHost((lfNodeHost) ? lfNodeHost : "127.0.0.1"),
	_lfNodePort(((lfNodePort > 0)&&(lfNodePort < 65536)) ? lfNodePort : 9980),
	_running(true),
	_ready(false),
	_storeOnlineState(storeOnlineState)
{
	_syncThread = std::thread([this]() {
		char controllerAddress[24];
		const uint64_t controllerAddressInt = _myId.address().toInt();
		_myId.address().toString(controllerAddress);
		std::string networksSelectorName("com.zerotier.controller.lfdb:"); networksSelectorName.append(controllerAddress); networksSelectorName.append("/network");
		std::string membersSelectorName("com.zerotier.controller.lfdb:"); membersSelectorName.append(controllerAddress); membersSelectorName.append("/network/member");

		httplib::Client htcli(_lfNodeHost.c_str(),_lfNodePort,600);
		std::ostringstream query;
		int64_t timeRangeStart = 0;
		while (_running) {
			{
				std::lock_guard<std::mutex> sl(_state_l);
				for(auto ns=_state.begin();ns!=_state.end();++ns) {
					if (ns->second.dirty) {
						nlohmann::json network;
						if (get(ns->first,network)) {
							nlohmann::json newrec;
							newrec["Selectors"] = {{ { { "Name",networksSelectorName },{ "Ordinal",ns->first } } }};
							newrec["Value"] = network.dump();
							newrec["OwnerPrivate"] = _lfOwnerPrivate;
							newrec["MaskingKey"] = controllerAddress;
							auto resp = htcli.Post("/make",newrec.dump(),"application/json");
							if (resp->status == 200) {
								ns->second.dirty = false;
							} else {
								fprintf(stderr,"ERROR: LFDB: %d from node (create/update network): %s" ZT_EOL_S,resp->status,resp->body.c_str());
							}
						}
					}

					for(auto ms=ns->second.members.begin();ms!=ns->second.members.end();++ms) {
						if ((_storeOnlineState)&&(ms->second.lastOnlineDirty)) {
						}

						if (ms->second.dirty) {
							nlohmann::json network,member;
							if (get(ns->first,network,ms->first,member)) {
								nlohmann::json newrec;
								newrec["Selectors"] = {{ { { "Name",networksSelectorName },{ "Ordinal",ns->first } },{ { "Name",membersSelectorName },{ "Ordinal",ms->first } } }};
								newrec["Value"] = member.dump();
								newrec["OwnerPrivate"] = _lfOwnerPrivate;
								newrec["MaskingKey"] = controllerAddress;
								auto resp = htcli.Post("/make",newrec.dump(),"application/json");
								if (resp->status == 200) {
									ms->second.dirty = false;
								} else {
									fprintf(stderr,"ERROR: LFDB: %d from node (create/update member): %s" ZT_EOL_S,resp->status,resp->body.c_str());
								}
							}
						}
					}
				}
			}

			query.clear();
			query
				<< '{'
					<< "\"Ranges\":[{"
						<< "\"Name\":\"" << networksSelectorName << "\","
						<< "\"Range\":[0,18446744073709551615]"
					<< "}],"
					<< "\"TimeRange\":[" << timeRangeStart << ",18446744073709551615],"
					<< "\"MaskingKey\":\"" << controllerAddress << "\","
					<< "\"Owners\":[\"" << _lfOwnerPublic << "\"]"
				<< '}';
			auto resp = htcli.Post("/query",query.str(),"application/json");
			if (resp->status == 200) {
				nlohmann::json results(OSUtils::jsonParse(resp->body));
				if ((results.is_array())&&(results.size() > 0)) {
					for(std::size_t ri=0;ri<results.size();++ri) {
						nlohmann::json &rset = results[ri];
						if ((rset.is_array())&&(rset.size() > 0)) {
							nlohmann::json &result = rset[0];
							if (result.is_object()) {
								nlohmann::json &record = result["Record"];
								if (record.is_object()) {
									const std::string recordValue = result["Value"];
									nlohmann::json network(OSUtils::jsonParse(recordValue));
									if (network.is_object()) {
										const std::string idstr = network["id"];
										const uint64_t id = Utils::hexStrToU64(idstr.c_str());
										if ((id >> 24) == controllerAddressInt) {
											std::lock_guard<std::mutex> sl(_state_l);
											_NetworkState &ns = _state[id];
											if (!ns.dirty) {
												nlohmann::json nullJson;
												_networkChanged(nullJson,network,false);
											}
										}
									}
								}
							}
						}
					}
				}
			} else {
				fprintf(stderr,"ERROR: LFDB: %d from node: %s" ZT_EOL_S,resp->status,resp->body.c_str());
			}

			query.clear();
			query
				<< '{'
					<< "\"Ranges\":[{"
						<< "\"Name\":\"" << networksSelectorName << "\","
						<< "\"Range\":[0,18446744073709551615]"
					<< "},{"
						<< "\"Name\":\"" << membersSelectorName << "\","
						<< "\"Range\":[0,18446744073709551615]"
					<< "}],"
					<< "\"TimeRange\":[" << timeRangeStart << ",18446744073709551615],"
					<< "\"MaskingKey\":\"" << controllerAddress << "\","
					<< "\"Owners\":[\"" << _lfOwnerPublic << "\"]"
				<< '}';
			resp = htcli.Post("/query",query.str(),"application/json");
			if (resp->status == 200) {
				nlohmann::json results(OSUtils::jsonParse(resp->body));
				if ((results.is_array())&&(results.size() > 0)) {
					for(std::size_t ri=0;ri<results.size();++ri) {
						nlohmann::json &rset = results[ri];
						if ((rset.is_array())&&(rset.size() > 0)) {
							nlohmann::json &result = rset[0];
							if (result.is_object()) {
								nlohmann::json &record = result["Record"];
								if (record.is_object()) {
									const std::string recordValue = result["Value"];
									nlohmann::json member(OSUtils::jsonParse(recordValue));
									if (member.is_object()) {
										const std::string nwidstr = member["nwid"];
										const std::string idstr = member["id"];
										const uint64_t nwid = Utils::hexStrToU64(nwidstr.c_str());
										const uint64_t id = Utils::hexStrToU64(idstr.c_str());
										if ((id)&&((nwid >> 24) == controllerAddressInt)) {
											std::lock_guard<std::mutex> sl(_state_l);
											auto ns = _state.find(nwid);
											if (ns != _state.end()) {
												_MemberState &ms = ns->second.members[id];
												if (!ms.dirty) {
													nlohmann::json nullJson;
													_memberChanged(nullJson,member,false);
												}
											}
										}
									}
								}
							}
						}
					}
				}
			} else {
				fprintf(stderr,"ERROR: LFDB: %d from node: %s" ZT_EOL_S,resp->status,resp->body.c_str());
			}

			timeRangeStart = time(nullptr) - 120; // start next query 2m before now to avoid losing updates
			_ready = true;

			for(int k=0;k<20;++k) { // 2s delay between queries for remotely modified networks or members
				if (!_running)
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	});
}

LFDB::~LFDB()
{
	_running = false;
	_syncThread.join();
}

bool LFDB::waitForReady()
{
	while (!_ready) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return true;
}

bool LFDB::isReady()
{
	return (_ready);
}

void LFDB::save(nlohmann::json *orig,nlohmann::json &record)
{
	if (orig) {
		if (*orig != record) {
			record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1;
		}
	} else {
		record["revision"] = 1;
	}

	const std::string objtype = record["objtype"];
	if (objtype == "network") {
		const uint64_t nwid = OSUtils::jsonIntHex(record["id"],0ULL);
		if (nwid) {
			nlohmann::json old;
			get(nwid,old);
			if ((!old.is_object())||(old != record)) {
				_networkChanged(old,record,true);
				{
					std::lock_guard<std::mutex> l(_state_l);
					_state[nwid].dirty = true;
				}
			}
		}
	} else if (objtype == "member") {
		const uint64_t nwid = OSUtils::jsonIntHex(record["nwid"],0ULL);
		const uint64_t id = OSUtils::jsonIntHex(record["id"],0ULL);
		if ((id)&&(nwid)) {
			nlohmann::json network,old;
			get(nwid,network,id,old);
			if ((!old.is_object())||(old != record)) {
				_memberChanged(old,record,true);
				{
					std::lock_guard<std::mutex> l(_state_l);
					_state[nwid].members[id].dirty = true;
				}
			}
		}
	}
}

void LFDB::eraseNetwork(const uint64_t networkId)
{
	// TODO
}

void LFDB::eraseMember(const uint64_t networkId,const uint64_t memberId)
{
	// TODO
}

void LFDB::nodeIsOnline(const uint64_t networkId,const uint64_t memberId,const InetAddress &physicalAddress)
{
	std::lock_guard<std::mutex> l(_state_l);
	auto nw = _state.find(networkId);
	if (nw != _state.end()) {
		auto m = nw->second.members.find(memberId);
		if (m != nw->second.members.end()) {
			m->second.lastOnlineTime = OSUtils::now();
			if (physicalAddress)
				m->second.lastOnlineAddress = physicalAddress;
			m->second.lastOnlineDirty = true;
		}
	}
}

} // namespace ZeroTier
