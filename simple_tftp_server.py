"""
Simple TFTP Server for testing
"""

import socket
import struct
import os

def handle_tftp_request(data, addr, sock):
    # Parse TFTP request
    opcode = struct.unpack('!H', data[:2])[0]
    
    if opcode == 1:  # RRQ (Read Request)
        filename_end = data.find(b'\x00', 2)
        filename = data[2:filename_end].decode('utf-8')
        print(f"TFTP Read Request for: {filename}")
        
        # Check if file exists
        file_path = os.path.join("C:\\tftpboot", filename)
        if os.path.exists(file_path):
            print(f"File found: {file_path}")
            # Send file data
            send_file(sock, file_path, addr)
        else:
            print(f"File not found: {file_path}")
            # Send error
            error_packet = struct.pack('!HH', 5, 1) + b'File not found\x00'
            sock.sendto(error_packet, addr)

def send_file(sock, file_path, addr):
    # Read file
    with open(file_path, 'rb') as f:
        file_data = f.read()
    
    # Send data in blocks
    block_num = 1
    pos = 0
    block_size = 512
    
    while pos < len(file_data):
        data_block = file_data[pos:pos+block_size]
        # Create DATA packet
        packet = struct.pack('!HH', 3, block_num) + data_block
        sock.sendto(packet, addr)
        
        # Wait for ACK
        try:
            ack_data, ack_addr = sock.recvfrom(1024)
            ack_opcode, ack_block = struct.unpack('!HH', ack_data[:4])
            if ack_opcode == 4 and ack_block == block_num:  # ACK
                print(f"Block {block_num} acknowledged")
                block_num += 1
                pos += block_size
            else:
                print(f"Unexpected ACK: {ack_opcode}, {ack_block}")
                break
        except socket.timeout:
            print("Timeout waiting for ACK")
            break
    
    # Send final empty block if file size is multiple of block_size
    if len(file_data) % block_size == 0:
        final_packet = struct.pack('!HH', 3, block_num) + b''
        sock.sendto(final_packet, addr)
        print(f"Sent final empty block {block_num}")
    
    print("File transfer completed")

def start_tftp_server():
    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', 69))
    sock.settimeout(30)  # 30 second timeout
    
    print("Simple TFTP Server started on port 69")
    print("TFTP Directory: C:\\tftpboot")
    print("Waiting for requests...")
    
    try:
        while True:
            data, addr = sock.recvfrom(1024)
            print(f"Received request from {addr}")
            handle_tftp_request(data, addr, sock)
    except KeyboardInterrupt:
        print("\nTFTP Server stopped")
    finally:
        sock.close()

if __name__ == "__main__":
    start_tftp_server()