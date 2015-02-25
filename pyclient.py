import zmq

context = zmq.Context()

# Socket to talk to server
print("Connecting to hello world server...")
socket = context.socket(zmq.REQ)
socket.connect("tcp://localhost:5555")
while True:
    socket.send(raw_input(""))
    # Get the reply.
    message = socket.recv()
    print "Received reply [ %s ]" % (message)

