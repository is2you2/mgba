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
            // 자유 타입 정보에 대해 브로드케스트
            default:
                handleBroadcast(clientUuid, msg);
                break;
        }
    });

    ws.on("close", () => {
        handleDisconnect(clientUuid);
    });
});

function handleJoin(clientUuid, msg) {
    const roomId = msg.uuid;

    if (!roomId) return;

    let room = rooms.get(roomId);

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

    // 본인에게만 joined 응답
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
    if (!client) return;

    const { roomId } = client;

    if (roomId) {
        const room = rooms.get(roomId);

        if (room) {
            room.clients.delete(clientUuid);

            // 방장인 경우 전체 종료
            if (room.host === clientUuid) {
                broadcast(room, {
                    type: "roomClosed",
                    message: "방장이 나가서 방이 종료되었습니다.",
                });

                rooms.delete(roomId);
            } else {
                const hostClient = clients.get(room.host);
                if (hostClient) {
                    send(hostClient.ws, {
                        type: "clientDisconnected",
                        uuid: clientUuid,
                    });
                }

                if (room.clients.size === 0) {
                    rooms.delete(roomId);
                }
            }
        }
    }

    clients.delete(clientUuid);
}

function broadcast(room, data, excludeUuid = null) {
    for (const clientId of room.clients) {
        if (excludeUuid && clientId === excludeUuid) continue;

        const client = clients.get(clientId);
        if (client) {
            send(client.ws, data);
        }
    }
}

function handleBroadcast(senderUuid, msg) {
    const sender = clients.get(senderUuid);
    if (!sender || !sender.roomId) return;

    const room = rooms.get(sender.roomId);
    if (!room) return;

    // 방 전체에게 전송 (sender 포함 여부는 선택)
    broadcast(room, msg);
}

console.log(`WebSocket Server : ${port}`);