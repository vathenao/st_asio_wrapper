
#include <boost/timer/timer.hpp>
#include <boost/tokenizer.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/lambda.hpp>

//configuration
#define SERVER_PORT		9528
//#define REUSE_OBJECT //use objects pool
//#define FORCE_TO_USE_MSG_RECV_BUFFER //force to use the msg recv buffer
//configuration

//use the following macro to control the type of packer and unpacker
#define PACKER_UNPACKER_TYPE	1
//1-default packer and unpacker, head(length) + body
//2-fixed length unpacker
//3-prefix and suffix packer and unpacker

#if 1 == PACKER_UNPACKER_TYPE
//#define DEFAULT_PACKER replaceable_packer
//#define DEFAULT_UNPACKER replaceable_unpacker
#elif 2 == PACKER_UNPACKER_TYPE
#define DEFAULT_UNPACKER fixed_length_unpacker
#elif 3 == PACKER_UNPACKER_TYPE
#define DEFAULT_PACKER prefix_suffix_packer
#define DEFAULT_UNPACKER prefix_suffix_unpacker
#endif

#include "../include/st_asio_wrapper_tcp_client.h"
using namespace st_asio_wrapper;

#ifdef _MSC_VER
#define atoll _atoi64
#endif

#define QUIT_COMMAND	"quit"
#define RESTART_COMMAND	"restart"
#define LIST_ALL_CLIENT	"list_all_client"
#define LIST_STATUS		"status"
#define SUSPEND_COMMAND	"suspend"
#define RESUME_COMMAND	"resume"

static bool check_msg;

///////////////////////////////////////////////////
//msg sending interface
#define TCP_RANDOM_SEND_MSG(FUNNAME, SEND_FUNNAME) \
void FUNNAME(const char* const pstr[], const size_t len[], size_t num, bool can_overflow = false) \
{ \
	size_t index = (size_t) ((boost::uint64_t) rand() * (size() - 1) / RAND_MAX); \
	at(index)->SEND_FUNNAME(pstr, len, num, can_overflow); \
} \
TCP_SEND_MSG_CALL_SWITCH(FUNNAME, void)
//msg sending interface
///////////////////////////////////////////////////

class test_socket : public st_connector
{
public:
	test_socket(boost::asio::io_service& io_service_) : st_connector(io_service_), recv_bytes(0), recv_index(0)
	{
#if 2 == PACKER_UNPACKER_TYPE
		dynamic_cast<fixed_length_unpacker*>(&*inner_unpacker())->fixed_length(1024);
#elif 3 == PACKER_UNPACKER_TYPE
		dynamic_cast<prefix_suffix_packer*>(&*inner_packer())->prefix_suffix("begin", "end");
		dynamic_cast<prefix_suffix_unpacker*>(&*inner_unpacker())->prefix_suffix("begin", "end");
#endif
	}

	boost::uint64_t get_recv_bytes() const {return recv_bytes;}
	operator boost::uint64_t() const {return recv_bytes;}

	void restart() {recv_bytes = recv_index = 0;}

protected:
	//msg handling
#ifndef FORCE_TO_USE_MSG_RECV_BUFFER //not force to use msg recv buffer(so on_msg will make the decision)
	//we can handle msg very fast, so we don't use recv buffer(return true)
	virtual bool on_msg(out_msg_type& msg) {handle_msg(msg); return true;}
#endif
	//we should handle msg in on_msg_handle for time-consuming task like this:
	virtual bool on_msg_handle(out_msg_type& msg, bool link_down) {handle_msg(msg); return true;}
	//msg handling end

private:
	void handle_msg(out_msg_ctype& msg)
	{
		recv_bytes += msg.size();
		if (check_msg && (msg.size() < sizeof(size_t) || 0 != memcmp(&recv_index, msg.data(), sizeof(size_t))))
			printf("check msg error: " size_t_format ".\n", recv_index);
		++recv_index;
	}

private:
	boost::uint64_t recv_bytes;
	size_t recv_index;
};

class test_client : public st_tcp_client_base<test_socket>
{
public:
	test_client(st_service_pump& service_pump_) : st_tcp_client_base<test_socket>(service_pump_) {}

	void restart() {do_something_to_all(boost::mem_fn(&test_socket::restart));}
	boost::uint64_t get_total_recv_bytes()
	{
		boost::uint64_t total_recv_bytes = 0;
		do_something_to_all(boost::ref(total_recv_bytes) += *boost::lambda::_1);
//		do_something_to_all(boost::ref(total_recv_bytes) += boost::lambda::bind(&test_socket::get_recv_bytes, &*boost::lambda::_1));

		return total_recv_bytes;
	}

	void close_some_client(size_t n)
	{
		while (n-- > 0)
			graceful_close(at(0));
//			graceful_close(at(0), false, false);
//			force_close(at(0));
		//this is a equivalence of calling i_server::del_client in st_server_socket_base::on_recv_error(see st_server_socket_base for more details).

		//if you just want to reconnect to the server, you should do it like this:
//		while (n-- > 0)
//			graceful_close(at(n), true); //if parameter 'reconnect' is true, st_tcp_client will not remove clients from object pool
//			graceful_close(at(n), true, false);
//			force_close(at(n), true);
	}

	///////////////////////////////////////////////////
	//msg sending interface
	//guarantee send msg successfully even if can_overflow equal to false, success at here just means putting the msg into st_tcp_socket's send buffer successfully
	TCP_RANDOM_SEND_MSG(safe_random_send_msg, safe_send_msg)
	TCP_RANDOM_SEND_MSG(safe_random_send_native_msg, safe_send_native_msg)
	//msg sending interface
	///////////////////////////////////////////////////
};

int main(int argc, const char* argv[])
{
	///////////////////////////////////////////////////////////
	printf("usage: test_client [<port=%d> [<ip=%s> [link num=16]]]\n", SERVER_PORT, SERVER_IP);

	size_t link_num = 16;
	if (argc > 3)
		link_num = std::min(MAX_OBJECT_NUM, std::max(atoi(argv[3]), 1));

	printf("exec: test_client " size_t_format "\n", link_num);
	///////////////////////////////////////////////////////////

	st_service_pump service_pump;
	test_client client(service_pump);
	for (size_t i = 0; i < link_num; ++i)
		client.add_client();

//	argv[2] = "::1" //ipv6
//	argv[2] = "127.0.0.1" //ipv4
	if (argc > 2)
		client.do_something_to_all(boost::bind(&test_socket::set_server_addr, _1, atoi(argv[1]), argv[2]));
	else if (argc > 1)
		client.do_something_to_all(boost::bind(&test_socket::set_server_addr, _1, atoi(argv[1]), SERVER_IP));

	service_pump.start_service(1);
	while(service_pump.is_running())
	{
		std::string str;
		std::getline(std::cin, str);
		if (str == QUIT_COMMAND)
			service_pump.stop_service();
		else if (str == RESTART_COMMAND)
		{
			service_pump.stop_service();
			service_pump.start_service(1);
		}
		else if (str == LIST_STATUS)
			printf("valid links: " size_t_format ", closed links: " size_t_format "\n", client.valid_size(), client.closed_object_size());
		//the following two commands demonstrate how to suspend msg dispatching, no matter recv buffer been used or not
		else if (str == SUSPEND_COMMAND)
			client.do_something_to_all(boost::bind(&test_socket::suspend_dispatch_msg, _1, true));
		else if (str == RESUME_COMMAND)
			client.do_something_to_all(boost::bind(&test_socket::suspend_dispatch_msg, _1, false));
		else if (str == LIST_ALL_CLIENT)
			client.list_all_object();
		else if (!str.empty())
		{
			if ('+' == str[0] || '-' == str[0])
			{
				size_t n = (size_t) atoi(boost::next(str.data()));
				if (0 == n)
					n = 1;

				if ('+' == str[0])
					for (; n > 0 && client.add_client(); ++link_num, --n);
				else
				{
					if (n > client.size())
						n = client.size();

					client.close_some_client(n);
					link_num = client.size();
				}

				continue;
			}

			size_t msg_num = 1024;
			size_t msg_len = 1024; //must greater than or equal to sizeof(size_t)
			char msg_fill = '0';
			char model = 0; //0 broadcast, 1 randomly pick one link per msg

			boost::char_separator<char> sep(" \t");
			boost::tokenizer<boost::char_separator<char> > tok(str, sep);
			BOOST_AUTO(iter, tok.begin());
			if (iter != tok.end()) msg_num = std::max((size_t) atoll(iter++->data()), (size_t) 1);

			bool native = false;
#if 1 == PACKER_UNPACKER_TYPE
			if (iter != tok.end()) msg_len = std::min(packer::get_max_msg_size(),
				std::max((size_t) atoi(iter++->data()), sizeof(size_t))); //include seq
#elif 2 == PACKER_UNPACKER_TYPE
			if (iter != tok.end()) ++iter;
			msg_len = 1024; //we hard code this because we fixedly initialized the length of fixed_length_unpacker to 1024
			native = true; //we don't have fixed_length_packer, so use packer instead, but need to pack msgs with native manner.
#elif 3 == PACKER_UNPACKER_TYPE
			if (iter != tok.end()) msg_len = std::min((size_t) MSG_BUFFER_SIZE,
				std::max((size_t) atoi(iter++->data()), sizeof(size_t)));
#endif
			if (iter != tok.end()) msg_fill = *iter++->data();
			if (iter != tok.end()) model = *iter++->data() - '0';

			unsigned percent = 0;
			boost::uint64_t total_msg_bytes;
			switch (model)
			{
			case 0:
				check_msg = true;
				total_msg_bytes = msg_num * link_num; break;
			case 1:
				check_msg = false;
				srand(time(NULL));
				total_msg_bytes = msg_num; break;
			default:
				total_msg_bytes = 0; break;
			}

			if (total_msg_bytes > 0)
			{
				printf("test parameters after adjustment: " size_t_format " " size_t_format " %c %d\n", msg_num, msg_len, msg_fill, model);
				puts("performance test begin, this application will have no response during the test!");

				client.restart();

				total_msg_bytes *= msg_len;
				boost::timer::cpu_timer begin_time;
				char* buff = new char[msg_len];
				memset(buff, msg_fill, msg_len);
				boost::uint64_t send_bytes = 0;
				for (size_t i = 0; i < msg_num; ++i)
				{
					memcpy(buff, &i, sizeof(size_t)); //seq

					switch (model)
					{
					case 0:
						native ? client.safe_broadcast_native_msg(buff, msg_len) : client.safe_broadcast_msg(buff, msg_len);
						send_bytes += link_num * msg_len;
						break;
					case 1:
						native ? client.safe_random_send_native_msg(buff, msg_len) : client.safe_random_send_msg(buff, msg_len);
						send_bytes += msg_len;
						break;
					default:
						break;
					}

					unsigned new_percent = (unsigned) (100 * send_bytes / total_msg_bytes);
					if (percent != new_percent)
					{
						percent = new_percent;
						printf("\r%u%%", percent);
						fflush(stdout);
					}
				}
				delete[] buff;

				while(client.get_total_recv_bytes() != total_msg_bytes)
					boost::this_thread::sleep(boost::get_system_time() + boost::posix_time::milliseconds(50));

				double used_time = (double) (begin_time.elapsed().wall / 1000000) / 1000;
				printf("\r100%%\ntime spent statistics: %.1f seconds.\n", used_time);
				printf("speed: %.0f(*2)kB/s.\n", total_msg_bytes / used_time / 1024);
			} // if (total_data_num > 0)
		}
	}

    return 0;
}

//restore configuration
#undef SERVER_PORT
#undef REUSE_OBJECT
#undef FORCE_TO_USE_MSG_RECV_BUFFER
#undef DEFAULT_PACKER
#undef DEFAULT_UNPACKER
//restore configuration
