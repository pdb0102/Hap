#include "Hap.h"


#include "srp\srp.h"

namespace Hap
{
	namespace Http
	{
		// current pairing
		SRP* srp = NULL;					// !NULL = pairing in progress, only one pairing at a time
		uint8_t srp_sess_key[32];			// SRP session key
		uint8_t srp_data[1024];
		sid_t srp_owner = sid_invalid;		// session owning the srp
		uint8_t srp_auth_count = 0;			// auth attempts counter

		// Open
		//	returns new session ID, 0..sid_max, or sid_invalid
		sid_t Server::Open()
		{
			for (sid_t sid = 0; sid < sizeofarr(_sess); sid++)
			{
				if (_sess[sid].isOpen())
					continue;

				_sess[sid].Open(sid);

				// open database
				_db.Open(sid);

				return sid;
			}

			return sid_invalid;
		}

		// Close
		//	returns true if opened session was closed
		bool Server::Close(sid_t sid)
		{
			if (sid > sid_max)
				return false;

			if (!_sess[sid].isOpen())
				return false;

			_db.Close(sid);

			_sess[sid].Close();

			// cancel current pairing if any
			if (srp != NULL && srp_owner == sid)
			{
				SRP_free(srp);
				srp = NULL;
				srp_owner = sid_invalid;
			}

			return true;
		}

		bool Server::Process(
			sid_t sid,
			void* ctx,
			std::function<int(sid_t sid, void* ctx, char* buf, uint16_t size)> recv,
			std::function<int(sid_t sid, void* ctx, char* buf, uint16_t len)> send
		)
		{
			if (sid > MaxHttpSessions)	// invalid sid
				return false;

			Session* sess = &_sess[sid];

			if (sid == MaxHttpSessions)	// too many sessions
			{
				// TODO: read request, create error response
				send(sid, ctx, sess->rsp.buf(), sess->rsp.len());
				return false;
			}

			sess->req.init();

			// read and parse the HTTP request
			uint16_t len = 0;		// total len of valid data in req
			while (true)
			{
				char* req = sess->req.buf() + len;
				uint16_t req_len = sess->req.size() - len;
				
				// read next portion of the request
				int l = recv(sid, ctx, req, req_len);
				if (l < 0)	// read error
				{
					Log("Http: Read Error\n");
					return false;
				}
				if (l == 0)
				{
					Log("Http: Read EOF\n");
					return false;
				}

				len += l;

				// TODO: it session is secured, decode the next portion, if whole block is received

				// parse HTTP request
				auto status = sess->req.parse(len);
				if (status == sess->req.Error)	// parser error
				{
					// TODO: make response Internal server error
					send(sid, ctx, sess->rsp.buf(), sess->rsp.len());
					return false;
				}

				if (status == sess->req.Success)
					break;

				// request incomplete - try reading more data
			}

			auto m = sess->req.method();
			Log("Method: '%.*s'\n", m.second, m.first);

			auto p = sess->req.path();
			Log("Path: '%.*s'\n", p.second, p.first);

			auto d = sess->req.data();

			for (size_t i = 0; i < sess->req.hdr_count(); i++)
			{
				auto n = sess->req.hdr_name(i);
				auto v = sess->req.hdr_value(i);
				Log("%.*s: '%.*s'\n", n.second, n.first, v.second, v.first);
			}

			if (m.second == 4 && strncmp(m.first, "POST", 4) == 0)
			{
				// POST
				//		/identify
				//		/pair-setup
				//		/pair-verify
				//		/pairings

				if (p.second == 9 && strncmp(p.first, "/identify", 9) == 0)
				{
					if (0 && _pairings.Count() == 0)
					{
						Log("Http: Exec unpaired identify\n");
						sess->rsp.start(HTTP_204);
						sess->rsp.end();
					}
					else
					{
						Log("Http: Unpaired identify prohibited when paired\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.add(ContentType, ContentTypeJson);
						sess->rsp.end("{\"status\":-70401}");
					}
				}
				else if (p.second == 11 && strncmp(p.first, "/pair-setup", 11) == 0)
				{
					int len;
					if (!sess->req.hdr(ContentType, ContentTypeTlv8))
					{
						Log("Http: Unknown or missing ContentType\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentLength, len))
					{
						Log("Http: Unknown or missing ContentLength\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else
					{
						sess->tlvi.parse(d.first, d.second);
						Log("PairSetup: TLV item count %d\n", sess->tlvi.count());

						Tlv::State state;
						if (!sess->tlvi.get(Tlv::Type::State, state))
						{
							Log("PairSetup: State not found\n");
						}
						else
						{
							switch (state)
							{
							case Tlv::State::M1:
								PairSetup_M1(sess);
								break;

							case Tlv::State::M3:
								PairSetup_M3(sess);
								break;

							case Tlv::State::M5:
								PairSetup_M5(sess);
								break;

							default:
								Log("PairSetup: Unknown state %d\n", (int)state);
							}
						}
					}
				}
				else if (p.second == 12 && strncmp(p.first, "/pair-verify", 12) == 0)
				{

				}
				else if (p.second == 9 && strncmp(p.first, "/pairings", 9) == 0)
				{

				}
				else
				{
					Log("Http: Unknown path %.*s\n", p.second, p.first);
					sess->rsp.start(HTTP_400);
					sess->rsp.end();
				}
			}
			else if (m.second == 3 && strncmp(m.first, "GET", 3) == 0)
			{
				// GET
				//		/accessories
				//		/characteristics
			}
			else if (m.second == 3 && strncmp(m.first, "PUT", 3) == 0)
			{
				// PUT
				//		/characteristics
			}

			send(sid, ctx, sess->rsp.buf(), sess->rsp.len());
			return true;
		}

		void Server::PairSetup_M1(Session* sess)
		{
			int rc;
			cstr* pub = NULL;

			Log("PairSetupM1\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create(sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M2);

			// verify that valid Method is present in input TLV
			Tlv::Method method;
			if (!sess->tlvi.get(Tlv::Type::Method, method))
			{
				Log("PairSetupM1: Method not found\n");
				goto RetErr;
			}
			if (method != Tlv::Method::PairSetupNonMfi)
			{
				Log("PairSetupM1: Invalid Method\n");
				goto RetErr;
			}

			// if the accessory is already paired it must respond Error_Unavailable
			if (_pairings.Count() != 0)
			{
				Log("PairSetupM1: Already paired, return Error_Unavailable\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unavailable);
				goto Ret;
			}

			// if accessory received more than 100 unsuccessfull auth attempts, respond Error_MaxTries
			if (srp_auth_count > 100)
			{
				Log("PairSetupM1: Too many auth attempts, return Error_MaxTries\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::MaxTries);
				goto Ret;
			}

			// if accessory is currently performing PairSetup with different controller, respond Error_Busy
			if (srp != NULL && srp_owner != sess->Sid())
			{
				Log("PairSetupM1: Already pairing, return Error_Busy\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Busy);
				goto Ret;
			}
			
			// create new pairing session
			srp = SRP_new(SRP6a_server_method());
			if (srp == NULL)
			{
				Log("PairSetupM1: SRP_new error\n");
				goto RetErr;
			}

			srp_owner = sess->Sid();
			srp_auth_count++;

			rc = SRP_set_username(srp, "Pair-Setup");
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_set_username error %d\n", rc);
				goto RetErr;
			}

			Hex("Username", srp->username->data, srp->username->length);

			uint8_t salt[16];
			t_random(salt, 16);
			rc = SRP_set_params(srp,
				srp_modulus, sizeof_srp_modulus,
				srp_generator, sizeof_srp_generator,
				salt, sizeof(salt)
			);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_set_params error %d\n", rc);
				goto RetErr;
			}

			Hex("Modulus", srp_modulus, sizeof_srp_modulus);
			Hex("Generator", srp_generator, sizeof_srp_generator);
			Hex("Salt", salt, sizeof(salt));

			rc = SRP_set_auth_password(srp, Hap::config.setup);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_set_auth_password error %d\n", rc);
				goto RetErr;
			}

			Hex("Username", Hap::config.setup, strlen(Hap::config.setup));

			rc = SRP_gen_pub(srp, &pub);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_gen_pub error %d\n", rc);
				goto RetErr;
			}

			Hex("ServerKey", pub->data, pub->length);

			sess->tlvo.add(Hap::Tlv::Type::PublicKey, pub->data, (uint16_t)pub->length);
			sess->tlvo.add(Hap::Tlv::Type::Salt, salt, sizeof(salt));

			goto Ret;

		RetErr:
			if (srp)
				SRP_free(srp);
			srp = NULL;
			srp_owner = sid_invalid;
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);
		
		Ret:
			if(pub != NULL)
				cstr_free(pub);

			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}
	
		void Server::PairSetup_M3(Session* sess)
		{
			int rc;
			uint16_t size;
			uint8_t* iosKey = srp_data;
			uint8_t* iosProof = srp_data + 384;
			uint16_t iosKey_size = 384;
			uint16_t iosProof_size = 64;
			cstr* key = NULL;
			cstr* rsp = NULL;

			Log("PairSetupM3\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create(sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M4);

			// verify that pairing is in progress on current session
			if (srp == nullptr || srp_owner != sess->Sid())
			{
				Log("PairSetupM3: No active pairing\n");
				goto RetErr;
			}

			// verify that required items are present in input TLV
			size = iosKey_size;
			if (!sess->tlvi.get(Tlv::Type::PublicKey, iosKey, iosKey_size))
			{
				Log("PairSetupM3: PublicKey not found\n");
				goto RetErr;
			}

			Hex("iosKey", iosKey, iosKey_size);

			size = iosProof_size;
			if (!sess->tlvi.get(Tlv::Type::Proof, iosProof, iosProof_size))
			{
				Log("PairSetupM3: Proof not found\n");
				goto RetErr;
			}

			Hex("iosProof", iosProof, iosProof_size);

			rc = SRP_compute_key(srp, &key, iosKey, iosKey_size);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM3: SRP_compute_key error %d\n", rc);
				goto RetErr;
			}

			Hap::Crypt::hkdf(
				(const uint8_t*)"Pair-Setup-Encrypt-Salt", sizeof("Pair-Setup-Encrypt-Salt") - 1,
				key->data, key->length,
				(const uint8_t*)"Pair-Setup-Encrypt-Info", sizeof("Pair-Setup-Encrypt-Info") - 1,
				srp_sess_key, sizeof(srp_sess_key));

			Hex("SessKey", srp_sess_key, sizeof(srp_sess_key));

			rc = SRP_verify(srp, iosProof, size);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM3: SRP_verify error %d\n", rc);
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
				goto Ret;
			}

			rc = SRP_respond(srp, &rsp);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM3: SRP_respond error %d\n", rc);
				goto RetErr;
			}

			Hex("Response", rsp->data, rsp->length);

			sess->tlvo.add(Hap::Tlv::Type::Proof, rsp->data, (uint16_t)rsp->length);

			goto Ret;

		RetErr:	// error, cancel current pairing, if this session owns it
			if (srp && srp_owner == sess->Sid())
			{
				SRP_free(srp);
				srp = NULL;
				srp_owner = sid_invalid;
			}
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			if (key != NULL)
				cstr_free(key);
			if (rsp != NULL)
				cstr_free(rsp);

			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::PairSetup_M5(Session* sess)
		{
			uint8_t* iosEncrypted;	// encrypted tata from iOS with tag attached
			uint8_t* iosTag;		// pointer to iOS tag
			uint8_t* iosTlv;		// decrypted TLV
			uint8_t* srvTag;		// calculated tag
			uint16_t iosTlv_size;

			Log("PairSetupM5\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create(sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M4);

			// verify that pairing is in progress on current session
			if (srp == nullptr || srp_owner != sess->Sid())
			{
				Log("PairSetupM5: No active pairing\n");
				goto RetErr;
			}

			// verify that required items are present in input TLV
			iosEncrypted = srp_data;
			iosTlv_size = sizeof(srp_data);
			if (!sess->tlvi.get(Tlv::Type::EncryptedData, iosEncrypted, iosTlv_size))
			{
				Log("PairSetupM5: EncryptedData not found\n");
				goto RetErr;
			}
			else
			{
				Hap::Tlv::Item id;
				Hap::Tlv::Item ltpk;
				Hap::Tlv::Item sign;

				// format std_data buffer
				iosTlv = iosEncrypted + iosTlv_size;
				iosTlv_size -= 16;	// strip off tag
				iosTag = iosEncrypted + iosTlv_size;
				srvTag = iosTlv + iosTlv_size;

				// decrypt iOS data
				Hap::Crypt::aead(Hap::Crypt::Decrypt, iosTlv, srvTag,
					srp_sess_key, (const uint8_t *)"\x00\x00\x00\x00PS-Msg05", 
					iosEncrypted, iosTlv_size);

				Hex("iosTlv", iosTlv, iosTlv_size);
				Hex("iosTag", iosTag, 16);
				Hex("srvTlv", srvTag, 16);

				if (memcmp(iosTag, srvTag, 16) != 0)
				{
					Log("PairSetupM5: authTag does not match\n");
					sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
					goto Ret;
				}

				Hap::Tlv::Parse<3> tlv(iosTlv, iosTlv_size);
				Log("PairSetupM5: TLV item count %d\n", tlv.count());

				if (!tlv.get(Hap::Tlv::Type::Identifier, id))
				{
					Log("PairSetupM5: Identifier not found\n");
					goto RetErr;
				}

				Hex("iosPairingId:", id.val(), id.len());

				if (!tlv.get(Hap::Tlv::Type::PublicKey, ltpk))
				{
					Log("PairSetupM5: PublicKey not found\n");
					goto RetErr;
				}

				Hex("iosLTPK:", ltpk.val(), ltpk.len());

				if (!tlv.get(Hap::Tlv::Type::Signature, sign))
				{
					Log("PairSetupM5: Signature not found\n");
					goto RetErr;
				}

				Hex("iosSignature:", sign.val(), sign.len());

				// TODO: verify iOS device signature

				if (!_pairings.Add(id, ltpk, _pairings.Admin))
				{
					Log("PairSetupM5: cannot add Pairing record\n");
					sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::MaxPeers);
					goto Ret;
				}


			}

			//goto Ret;

		RetErr:	// error, cancel current pairing, if this session owns it
			if (srp && srp_owner == sess->Sid())
			{
				SRP_free(srp);
				srp = NULL;
				srp_owner = sid_invalid;
			}
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:

			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}
	}
}
