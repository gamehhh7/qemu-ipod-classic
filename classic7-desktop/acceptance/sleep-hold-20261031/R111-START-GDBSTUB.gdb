set pagination off
set confirm off
printf "ENABLE_LOCAL_GUEST_DEBUGGER\n"
p gdbserver_start("tcp:127.0.0.1:15870")
detach
quit
