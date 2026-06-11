const WebSocket = require("ws");
const { v4: uuidv4 } = require("uuid");

const port = 8080;

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

wss.on("connection", (ws) => {
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
    });
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
            room.clients.delete(clientUuid);

            // host 나가면 첫 번째 사용자에게 host 위임
            if (room.host === clientUuid) {
                const nextHost = room.clients.values().next().value;

                if (nextHost) {
                    room.host = nextHost;

                    for (const memberId of room.clients) {
                        const member = clients.get(memberId);

                        send(member.ws, {
                            type: "hostChanged",
                            host: nextHost,
                        });
                    }
                } else {
                    rooms.delete(roomId);
                }
            } else if (room.clients.size === 0) {
                rooms.delete(roomId);
            }
        }
    }

    clients.delete(clientUuid);
}

console.log(`WebSocket Server : ${port}`);