#include <SDKDDKVer.h>

#include <tchar.h>
#include <stdio.h> 
#include <string>
#include <iostream>

#include "Hap.h"

// convert bin to hex, sizeof(s) must be >= size*2 + 1
void bin2hex(uint8_t* buf, size_t size, char* s)
{
	static const char h[] = "0123456789ABCDEF";

	while (size-- > 0)
	{
		uint8_t b = *buf++;

		*s++ = h[b >> 4];
		*s++ = h[b & 0xF];
	}
	*s++ = 0;
}

class MyAccessoryInformation : public Hap::AccessoryInformation
{
public:
	MyAccessoryInformation()
	{
		_identify.onWrite([this](Hap::Obj::wr_prm& p, Hap::Characteristic::Identify::V v) -> void {
			Log("MyAccessoryInformation: write Identify\n");
		});
	}

	void config()
	{
		_manufacturer.Value(Hap::config->manufacturer);
		_model.Value(Hap::config->model);
		_name.Value(Hap::config->name);
		_serialNumber.Value(Hap::config->serialNumber);
		_firmwareRevision.Value(Hap::config->firmwareRevision);
	}

} myAis;

class MyLb : public Hap::Lightbulb
{
private:
	Hap::Characteristic::Name _name;
	int _n;
public:
	MyLb(Hap::Characteristic::Name::V name, int n) : _n(n)
	{

		AddName(_name);
		_name.Value(name);

		On().onRead([this](Hap::Obj::rd_prm& p) -> void {
			Log("MyLb%d: read On: %d\n", _n, On().Value());
		});

		On().onWrite([this](Hap::Obj::wr_prm& p, Hap::Characteristic::On::V v) -> void {
			Log("MyLb%d: write On: %d -> %d\n", _n, On().Value(), v);
		});
	}

} myLb1("Light-1", 1), myLb2("Light-2", 2);

class MyAcc : public Hap::Accessory<3>
{
public:
	MyAcc() : Hap::Accessory<3>()
	{
		AddService(&myAis);
		AddService(&myLb1);
		AddService(&myLb2);
	}

} myAcc;

class MyDb : public Hap::DbStatic<1>
{
public:
	MyDb()
	{
		AddAcc(&myAcc);
	}

	// db initialization:
	void Init(Hap::iid_t aid)
	{
		// assign instance IDs
		myAcc.setId(aid);

		// config AIS
		myAis.config();
	}

} db;

class MyPairings : public Hap::Pairings
{
public:
	void Reset()
	{
		init();
	}

	bool Restore()
	{
		return false;
	}

	bool Save(FILE* f)
	{
		if (f == NULL)
			return false;

		char* key = new char[Hap::Controller::KeyLen * 2 + 1];

		bool comma = false;
		for (int i = 0; i < sizeofarr(_db); i++)
		{
			Hap::Controller* ios = &_db[i];

			if (ios->perm == Hap::Controller::None)
				continue;

			bin2hex(ios->key, ios->KeyLen, key);

			fprintf(f, "\t\t%c{\"id\":\"%.*s\",\"key\":\"%s\",\"perm\":%d}\n", comma ? ',' :' ', ios->idLen, ios->id, key, ios->perm);
			comma = true;
		}

		delete[] key;

		return true;
	}
};

class MyCrypto : public Hap::Crypt::Ed25519
{
public:
	void Reset()
	{
		init();
	}

	bool Restore()
	{
		return false;
	}

	bool Save(FILE* f)
	{
		if (f == NULL)
			return false;

		char* s = new char[PrvKeySize * 2 + 1];

		bin2hex(_pubKey, PubKeySize, s);
		fprintf(f, "\t\t \"%s\"\n", s);

		bin2hex(_prvKey, PrvKeySize, s);
		fprintf(f, "\t\t,\"%s\"\n", s);

		delete[] s;
		
		return true;
	}
};

// configuration data of this accessory server
//	implements save/restote to/from persistent storage 
class MyConfig : public Hap::Config
{
public:
	MyPairings pairings;
	MyCrypto keys;

	MyConfig(const char* fileName)
		: _fileName(fileName)
	{}

	bool Exec(Action action)
	{
		switch (action)
		{
		case Restore:
			Log("Config: restore from %s\n", _fileName);

		case Reset:
			Log("Config: reset\n");

			name = "esp32test";				// const char* name;	// Accessory name - used as initial Bonjour name and as	Accessory Information Service name of aid=1
			model = "TestModel";			// const char* model;	// Model name (Bonjour and AIS)
			manufacturer = "TestMaker";		// Manufacturer- used by AIS (Accessory Information Service)
			serialNumber = "0001";			// Serial number in arbitrary format
			firmwareRevision = "0.1";		// Major[.Minor[.Revision]]
			deviceId = "00:11:22:33:44:55";	// const char* deviceId;		// Device ID (XX:XX:XX:XX:XX:XX, generated new on factory reset)
			configNum = 1;					// uint32_t configNum;			// Current configuration number, incremented on db change
			categoryId = 5;					// uint8_t categoryId;			// category identifier
			statusFlags = 0					// uint8_t statusFlags;			// status flags
				| Hap::Bonjour::NotPaired
				| Hap::Bonjour::NotConfiguredForWiFi;
			setupCode = "000-11-000";		// const char* setupCode	// Setup code
			port = swap_16(7889);			// uint16_t port;		// TCP port of HAP service
			BCT = 0;

			pairings.Reset();
			keys.Reset();

		case Save:
		{
			FILE* f = fopen(_fileName, "w+");

			if (f == NULL)
			{
				Log("Config: cannot open %s for write\n", _fileName);
				return false;
			}

			Log("Config: save to %s\n", _fileName);

			fprintf(f, "{\n");
			fprintf(f, "\t\"name\":\"%s\",\n", name);
			fprintf(f, "\t\"model\":\"%s\",\n", model);
			fprintf(f, "\t\"manufacturer\":\"%s\",\n", manufacturer);
			fprintf(f, "\t\"serialNumber\":\"%s\",\n", serialNumber);
			fprintf(f, "\t\"firmwareRevision\":\"%s\",\n", firmwareRevision);
			fprintf(f, "\t\"deviceId\":\"%s\",\n", deviceId);
			fprintf(f, "\t\"configNum\":%d,\n", configNum);
			fprintf(f, "\t\"categoryId\":%d,\n", categoryId);
			fprintf(f, "\t\"statusFlags\":%d,\n", statusFlags);
			fprintf(f, "\t\"setupCode\":\"%s\",\n", setupCode);
			fprintf(f, "\t\"port\":\"%d\",\n", swap_16(port));
			fprintf(f, "\t\"keys\":[\n");
			keys.Save(f);
			fprintf(f, "\t],\n");
			fprintf(f, "\t\"pairings\":[\n");
			pairings.Save(f);
			fprintf(f, "\t]\n");
			fprintf(f, "}\n");

			fclose(f);
			return true;
		}
		}

		return false;
	}

private:
	const char* _fileName;
};

MyConfig myConfig("TestAccessory.hap");
Hap::Config* Hap::config = &myConfig;

// statically allocated storage for HTTP processing
//	Our implementation is single-threaded so only one set of buffers.
//	The http server uses this buffers only during processing a request.
//	All session-persistend data is kept in Session objects.
Hap::BufStatic<char, Hap::MaxHttpFrame * 2> http_req;
Hap::BufStatic<char, Hap::MaxHttpFrame * 4> http_rsp;
Hap::BufStatic<char, Hap::MaxHttpFrame * 1> http_tmp;
Hap::Http::Server::Buf buf = { http_req, http_rsp, http_tmp };
Hap::Http::Server http(buf, db, myConfig.pairings, myConfig.keys);

int main()
{
	// create servers
	Hap::Mdns* mdns = Hap::Mdns::Create();
	Hap::Tcp* tcp = Hap::Tcp::Create(&http);

	// restore configuration
	myConfig.Exec(Hap::Config::Restore);
	myConfig.Update = [mdns]() -> void {

		bool mdnsUpdate = false;

		// see if status flag must change
		bool paired = myConfig.pairings.Count() != 0;
		if (paired && (Hap::config->statusFlags & Hap::Bonjour::NotPaired))
		{
			Hap::config->statusFlags &= ~Hap::Bonjour::NotPaired;
			mdnsUpdate = true;
		}
		else if (!paired && !(Hap::config->statusFlags & Hap::Bonjour::NotPaired))
		{
			Hap::config->statusFlags |= Hap::Bonjour::NotPaired;
			mdnsUpdate = true;
		}
		
		myConfig.Exec(Hap::Config::Save);

		if (mdnsUpdate)
			mdns->Update();
	};

	// init static objects
	db.Init(1);

#if 1
	// start servers
	mdns->Start();
	tcp->Start();

	// wait for interrupt
	char c;
	std::cin >> c;

	// stop servers
	tcp->Stop();
	mdns->Stop();

#else
	Hap::sid_t sid = http.Open();

	static Hap::BufStatic<char, 4096> buf;
	char* s = buf.ptr();
	int l, len = buf.len();

	l = db.getDb(sid, s, len);
	s[l] = 0;
	printf("sizeof(srv)=%d  db '%s'\n", sizeof(db), s);

	//static const char wr[] = "{\"characteristics\":[{\"aid\":1,\"iid\":2,\"value\":true,\"ev\":true},{\"aid\":3,\"iid\":8,\"ev\":true}]}";
	const char wr[] = "{\"characteristics\":[{\"aid\":1,\"iid\":9,\"value\":true,\"ev\":true},{\"aid\":1,\"iid\":11,\"value\":true,\"ev\":true}]}";

	l = len;
	auto rc = db.Write(sid, wr, sizeof(wr) - 1, s, l);
	Log("Write: %s  rsp '%.*s'\n", Hap::Http::StatusStr(rc), l, s);

	l = len;
	memset(s, 0, l);
	rc = db.getEvents(sid, s, l);
	Log("Events: %s  rsp %d '%.*s'\n", Hap::Http::StatusStr(rc), l, l, s);

	l = len;
	memset(s, 0, l);
	rc = db.getEvents(sid, s, l);
	Log("Events: %s  rsp %d '%.*s'\n", Hap::Http::StatusStr(rc), l, l, s);

	http.Close(sid);

	//sha2_test();
	//srp_test();
	//return 0;

#endif

#if 0
	static const char wr[] = "{\"characteristics\":[{\"aid\":1,\"iid\":2,\"value\":true,\"ev\":true},{\"aid\":3,\"iid\":8,\"ev\":true}]}";
	//	const char wr[] = "{\"characteristics\":[{\"aid\":1,\"iid\":8,\"value\":true}]}";
	static char rsp[256];
	int rsp_size = sizeof(rsp);

	auto rc = db.Write(sid, wr, sizeof(wr) - 1, rsp, rsp_size);
	Log("Write: %s  rsp '%.*s'\n", Hap::Http::StatusStr(rc), rsp_size, rsp);

	static const char rd[] = "id=1.2,3.1&ev=1&meta=1&perms=1&type=1";
	rsp_size = sizeof(rsp);
	rc = db.Read(sid, rd, sizeof(rd) - 1, rsp, rsp_size);
	Log("Read: %s  rsp '%.*s'\n", Hap::Http::StatusStr(rc), rsp_size, rsp);

	rsp_size = sizeof(rsp);
	rc = db.getEvents(sid, rsp, rsp_size);
	Log("Events: %s  rsp %d '%.*s'\n", Hap::Http::StatusStr(rc), rsp_size, rsp_size, rsp);

	rsp_size = sizeof(rsp);
	rc = db.getEvents(sid, rsp, rsp_size);
	Log("Events: %s  rsp %d '%.*s'\n", Hap::Http::StatusStr(rc), rsp_size, rsp_size, rsp);

	tcpServer();
	srv.Process(sid, nullptr,
	[](Hap::sid_t sid, void* ctx, uint8_t* buf, uint16_t& size) -> bool {

		static bool first = true;
		static const char req[] = "";

		if (first)
		{
			size = sizeof(req);
			memcpy(buf, req, size);
			first = false;
			return true;
		}
		return false;
	},
	[](Hap::sid_t sid, void* ctx, uint8_t* buf, uint16_t len, bool close) -> bool {
		return true;
	});

#endif


	return 0;
}

#include <time.h>
#include <stdlib.h>

extern "C" {
	void t_stronginitrand()
	{
		srand((unsigned)time(NULL));
	}

	void t_random(unsigned char* data, unsigned size)
	{

		for (unsigned i = 0; i < size; i++)
		{
			*data++ = /*rand()*/ i & 0xFF;
		}
	}
}

#if 0

Hap::Property::Type ty("asdfgh");
Hap::Property::InstanceId iid;
Hap::Property::EventNotifications en(false);
Hap::Property::LinkedServices<5> ls;
Hap::Property::MinValue<Hap::FormatId::Int> mvi(10);
Hap::Property::MinValue<Hap::FormatId::Float> mvf(1.1);

//	printf("sizeof(c1)=%d  type '%s'  iid %lld\n",
//		sizeof(c1), c1.Type(), c1.Iid());

//	printf("sizeof(c2)=%d  type '%s'  iid %lld\n",
//		sizeof(c2), c2.Type(), c2.Iid());

//	Hap::Characteristic::On on(3);
//	on.on();
//	int l = on.getDb(str, sizeof(str) - 1);
//	str[l] = 0;

//	printf("sizeof(on)=%d  type '%s'  iid %lld  val %d  db '%s'\n",
//		sizeof(on), on.Type(), on.Iid(), on.Value(), str);

//	lb.On().on();

//	l = lb.getDb(str, sizeof(str) - 1);
//	str[l] = 0;
//	printf("sizeof(lb)=%d  type '%s'  iid %lld  db '%s'\n",
//		sizeof(lb), lb.Type().get(), lb.Iid().get(), str);

//	Hap::Accessory<Hap::Property::Type, Hap::Property::InstanceId> acc(ty, iid);

//	printf("ty=%s  sizeof(ty)=%d FormatId %d  Size %d  Length %d  Format '%s'  Key '%s'\n",
//		ty.get(), sizeof(ty), ty.formatId(), ty.size(), ty.length(), ty.format(), ty.key());

//	printf("en=%d  sizeof(en)=%d FormatId %d  Size %d  Length %d  Format '%s'  Key '%s'\n",
//		en.get(), sizeof(en), en.formatId(), en.size(), en.length(), en.format(), en.key());
en.set(true);
printf("en=%d\n", en.get());

Hap::Property::Permissions pm(Hap::Property::Permissions::PairedRead | Hap::Property::Permissions::PairedWrite);
printf("isPairedWrite: %d  isEvent: %d\n",
	pm.isEnabled(Hap::Property::Permissions::PairedWrite),
	pm.isEnabled(Hap::Property::Permissions::Events)
);

//	ls[1] = 1234;
ls.set(2, 3456);
//	printf("sizeof(ls)=%d FormatId %d  Size %d  Length %d  ls[0] %lld  ls[1] %lld  ls[2] %lld\n",
//		sizeof(ls), ls.formatId(), ls.size(), ls.length(), ls[0], ls[1], ls.get(2));

printf("mvi %d  mvf %g\n", mvi.get(), mvf.get());

#endif

