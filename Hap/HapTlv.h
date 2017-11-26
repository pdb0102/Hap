#ifndef _HAP_TLV_H_
#define _HAP_TLV_H_

// TLV parser/creator

namespace Hap
{
	namespace Tlv
	{
		enum class Method : uint8_t
		{
			Reserved = 0,
			PairSetup = 1,
			PairVerivy = 2,
			AddPairing = 3,
			RemovePairing = 4,
			ListPairing = 5,
		};

		enum class State : uint8_t
		{
			Unknown = 0,
			M1 = 1,
			M2 = 2,
			M3 = 3,
			M4 = 4,
			M5 = 5,
			M6 = 6,
		};

		enum class Error : uint8_t
		{
			Unknown = 0x01,
			Authentication = 0x02,
			Backoff = 0x03,
			MaxPeers = 0x04,
			MaxTries = 0x05,
			Unavailable = 0x06,
			Busy = 0x07,
		};

		enum class Type : uint8_t
		{
			Method = 0x00,
			Identifier = 0x01,
			Salt = 0x02,
			PublicKey = 0x03,
			Proof = 0x04,
			EncryptedData = 0x05,
			State = 0x06,
			Error = 0x07,
			RetryDelay = 0x08,
			Certificate = 0x09,
			Signature = 0x0A,
			Permissions = 0x0B,
			FragmentData = 0x0C,
			Fragmentlast = 0x0D,
			Separator = 0xFF,
			Invalid = 0xFE
		};

		template<int MaxTlv>	// Max number of TLVs expected, parsing stops when MaxTlv is reached
		class Parse
		{
		private:
			const uint8_t* _buf;	// data buffer containing TLVs
			uint16_t _len;			// length of the buffer
			uint16_t _off[MaxTlv];	// offsets
			uint8_t _cnt;			// number of TLVs found

		public:
			// parse passed in buffer
			uint8_t parse(const char* buf, uint16_t len)
			{
				_buf = (const uint8_t*)buf;
				_len = len;
					
				const uint8_t* b = _buf;
				uint16_t l = _len;
				uint8_t i;

				_cnt = 0;
				for (i = 0; i < sizeofarr(_off); i++)
				{
					if (l < 2)
						break;

					_off[_cnt++] = b - _buf;

					Type t = Type(b[0]);
					uint16_t s = b[1];

					Log("Tlv: type %d  length %d\n", int(t), s);

					s += 2;
					if (s > l)	// this Tlv spans beyond the buffer, limit its size
						s = l;
					b += s;
					l -= s;
				}

				return _cnt;
			}

			// return number of items found in this TLV
			uint8_t count()
			{
				return _cnt;
			}

			// return type of item i
			Type type(uint8_t i)
			{
				if (i >= _cnt)
					return Type::Invalid;

				return Type(_buf[_off[i]]);
			}

			// return length of item i
			uint8_t length(uint8_t i)
			{
				if (i >= _cnt)
					return 0;

				return _buf[_off[i] + 1];
			}

			// return pointer to value of item i
			const uint8_t* value(uint8_t i)
			{
				if (i >= _cnt)
					return 0;

				return _buf + _off[i] + 2;
			}

			// extract low-endian integer from item i
			int getInt(uint8_t i)
			{
				int r = 0;
				uint8_t l = length(i);
				const uint8_t* v = value(i);

				for (int i = 0; i < l; i++)
					r |= (*v++) << (8 * i);

				return r;
			}

			// extract int/enum value from item with type t
			//	return false if item does not exist
			template<typename T>
			bool get(Type t, T& v)
			{
				for (uint8_t i = 0; i < _cnt; i++)
				{
					if (type(i) == t)
					{
						v = T(getInt(i));
						return true;
					}
				}
				return false;
			}

			// extract data bytes from single TLV
			//	the data buffer must be at least length(i) bytes long
			void getBytes(uint8_t i, uint8_t* data)
			{
				memcpy(data, value(i), length(i));
			}

			// extract multi-TLV data starting from TLV i
			//	size contains buffer size
			//	on return contains length of data copied to the buffer
			//	func returns false if data does not fit into the buffer
			bool getData(uint8_t i, uint8_t* data, uint16_t& size)
			{
				uint8_t* d = data;
				uint16_t max = size;
				Type t = type(i);

				while (true)
				{
					if (type(i) != t)	// different type or no more TLVs
					{
						size = d - data;
						return true;
					}

					if (length(i) < max)
						return false;

					getBytes(i, d);

					d += length(i);
					max -= length(i);

					i++;
				}
			}
		};

		class Create
		{
		private:
			uint8_t* _buf;		// data buffer for TLV
			uint16_t _size;		// size the buffer
			uint16_t _len;		// length of valid TLV

		public:
			void create(char* buf, uint16_t size)
			{
				_buf = (uint8_t *)buf; 
				_size = size;
				_len = 0;
			}

			uint16_t length()
			{
				return _len;
			}

			// add Integer, use as many bytes as necessary
			bool addInt(Type t, int v)
			{
				// ensure space for at least 3 bytes
				if (_size - _len < 3)
					return false;

				uint8_t* b = _buf + _len;	// tlv start
				*b++ = uint8_t(t);			// store type
				uint8_t* l = b++;			// save length byte pointer

				*l = 0;						// zero length
				do
				{
					*b++ = v & 0xFF;		// store next byte of integer (starting from lsb)
					(*l)++;					// increment length
					
					if (b == _buf + _size)
						return false;		// doesn't fit

					v >>= 8;				// move to next byte

				} while (v != 0);

				_len = b - _buf;
				return true;
			}

			template<typename T>
			bool add(Type t, T v)
			{
				return addInt(t, int(v));
			}

			// Add bytes, up to 255
			bool addBytes(Type t, uint8_t* d, uint8_t l)
			{
				// ensure there is space
				if (_size - _len < l + 2)
					return false;

				uint8_t* b = _buf + _len;	// tlv start
				*b++ = uint8_t(t);			// store type
				*b++ = l;					// store length
				memcpy(b, d, l);			// store data
				b += l;

				_len = b - _buf;
				return true;
			}

			// Add data, span multiple items as necessary
			bool addData(Type t, uint8_t* data, uint16_t len)
			{
				uint8_t* d = data;

				while (len > 0)
				{
					uint8_t l = len > 255 ? 255 : len;

					if (!addBytes(t, d, l))
						return false;

					d += l;
					len -= l;
				}

				return true;
			}
		};
	}
}

#endif