#ifndef _HAP_TCP_H_
#define _HAP_TCP_H_

// Interface to system-dependent TCP server implementation

namespace Hap
{
	class Tcp
	{
	protected:
		Hap::Config* _cfg;
		Hap::Http::Server* _http;
	public:
		static Tcp* Create(Hap::Config* cfg, Hap::Http::Server* _http);
		virtual bool Start() = 0;
		virtual void Stop() = 0;
	};
}

#endif