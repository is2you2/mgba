const WebSocket = require("ws");
const { v4: uuidv4 } = require("uuid");

const port = 4000;

const wss = new WebSocket.Server({
    port: port,
});

//
// rooms
//
// roomId => {
//   host: clientUuid,
//   clients: Set<clientUuid>
// }
//
const rooms = new Map();

//
// clients
//
// clientUuid => {
//   ws,
//   roomId
// }
//
const clients = new Map();

function send(ws, data) {
    if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(data));
    }
}

wss.on("connection", (ws, req) => {
    const clientUuid = uuidv4();

    clients.set(clientUuid, {
        ws,
        roomId: null,
    });

    // 접속 즉시 uuid 전달
    send(ws, {
        type: "connected",
        uuid: clientUuid,
    });

    ws.on("message", (raw) => {
        let msg;

        try {
            msg = JSON.parse(raw.toString());
        } catch {
            return;
        }


        switch (msg.type) {
            case "join":
                // 로컬망 빠른 연결을 위해 발송자 ip주소를 수집
                msg['senderAddr'] = req.socket.remoteAddress;
                handleJoin(clientUuid, msg);
                break;
            case "msg":
                handlePrivateMessage(clientUuid, msg);
                break;
        }
    });

    ws.on("close", () => {
        handleDisconnect(clientUuid);
    });
});

function handleJoin(clientUuid, msg) {
    const roomId = msg.uuid;

    if (!roomId) {
        return;
    }

    let room = rooms.get(roomId);

    // 방 없으면 생성 + 자신이 host
    if (!room) {
        room = {
            host: clientUuid,
            data: msg.data,
            clients: new Set(),
        };

        rooms.set(roomId, room);
    }

    room.clients.add(clientUuid);

    const client = clients.get(clientUuid);
    client.roomId = roomId;

    send(client.ws, {
        type: "joined",
        roomId,
        host: room.host,
        isHost: room.host === clientUuid,
        data: room.data,
    });

    // 참여자라면 방장에게도 메시지 발송
    if (room.host != clientUuid) {
        const hostClient = clients.get(room.host);
        if (hostClient) {
            send(hostClient.ws, {
                type: "msg",
                from: clientUuid,
                data: {
                    type: "joined",
                    roomId,
                    senderAddr: msg['senderAddr'],
                }
            });
        }
    }
}

function handlePrivateMessage(senderUuid, msg) {
    const {
        to,
        data,
    } = msg;

    const sender = clients.get(senderUuid);

    if (!sender || !sender.roomId) {
        return;
    }

    const room = rooms.get(sender.roomId);

    if (!room) {
        return;
    }

    // 같은 방 사용자만 허용
    if (!room.clients.has(to)) {
        return;
    }

    const target = clients.get(to);

    if (!target) {
        return;
    }

    send(target.ws, {
        type: "msg",
        from: senderUuid,
        data,
    });
}

function handleDisconnect(clientUuid) {
    const client = clients.get(clientUuid);

    if (!client) {
        return;
    }

    const { roomId } = client;

    if (roomId) {
        const room = rooms.get(roomId);

        if (room) {
            if (room.host === clientUuid) {
                // 방장이 나가면 모든 클라이언트 퇴출
                for (const memberId of room.clients) {
                    const member = clients.get(memberId);
                    if (member) {
                        send(member.ws, {
                            type: "roomClosed",
                            message: "방장이 나가서 방이 종료되었습니다.",
                        });
                    }
                }
                rooms.delete(roomId);
            } else {
                // 방장이 아닌 경우 단순히 클라이언트 제거
                room.clients.delete(clientUuid);

                if (room.clients.size === 0) {
                    rooms.delete(roomId);
                }
            }
        }
    }

    clients.delete(clientUuid);
}

console.log(`WebSocket Server : ${port}`);