from flask import Flask, request, jsonify, render_template_string, redirect, url_for
import webbrowser
import sys
from copy import deepcopy
import requests
import socket
import subprocess
import os
import json
import datetime
import time
import uuid
import random
import threading
import base64
import random
from hashlib import sha256
import hmac
import argparse

json_b64 = lambda x: base64.b64encode(json.dumps(x).encode()).decode("utf-8", "ignore")
# Lists of adjectives and nouns
adjectives = [
    "Swift", "Brave", "Bright", "Calm", "Daring", "Eager", "Fancy", "Gentle", "Happy", "Jolly",
    "Mighty", "Noble", "Quick", "Wise", "Clever", "Bold", "Charming", "Radiant", "Lively", "Vivid",
    "Sleek", "Witty", "Graceful", "Dynamic", "Fiery", "Vibrant", "Glorious", "Majestic", "Serene", "Playful"
]

nouns = [
    "Lion", "Tiger", "Bear", "Fox", "Hawk", "Eagle", "Shark", "Wolf", "Falcon", "Panther",
    "Dragon", "Leopard", "Puma", "Viper", "Griffin", "Phoenix", "Jaguar", "Cheetah", "Cobra", "Raven",
    "Orca", "Cougar", "Owl", "Stallion", "Bull", "Crane", "Bison", "Lynx", "Condor", "Pelican"
]

NOT_READY = "not_ready"
READY = "ready"
hosts_processes = {}
users = {}
rooms = {} #{roomId : {roomKey, owner_uid, creater_nick, scene, sessionId, hostIp, hostPort, users}}
roomsList = []

globalparams = {
  "encryption": True,
  "game_server_dir": os.path.abspath(os.path.join(os.path.dirname(__file__), "../../game")),
  "external_fixed_host_ip": None,
  "exe_path": 'win32/outer_space-ded-dev.exe' if sys.platform == 'win32' else 'linux64/outer_space-ded-dev',
  "portrange_base":20010,
  "portrange_num":90,
}
if sys.platform == "darwin":
  globalparams["exe_path"] = 'macOS/outer_space-ded-dev'


def uuid_str_int32():
  return str(uuid.uuid4().int & (1<<32)-1)

def uuid_str_int31():
  return str(random.randint(0, (1 << 31) - 1))

def c_time():
  return time.monotonic()

def generate_username():
    # Choose a random adjective and noun
    adjective = random.choice(adjectives)
    noun = random.choice(nouns)

    # Combine them to form a username
    username = adjective + noun

    return username

def get_master_server_ip():
    server_ip = request.host.split(':')[0]  # Split to handle cases with port numbers
    print(f'server_ip IP address is {server_ip}')
    return server_ip

def get_host_ip():
    if globalparams["external_fixed_host_ip"]:
      return globalparams["external_fixed_host_ip"]
    return get_master_server_ip()

def get_free_udp_socket():
    for i in range(globalparams["portrange_num"]):
        port = globalparams["portrange_base"] + i
        if port in hosts_processes:
            continue
        return port

def get_game_server_path():
    return globalparams["game_server_dir"]

def get_game_server_exe():
    return globalparams["exe_path"]

def mk_user_info_for_room(userid, nick=None, status=NOT_READY):
    return {
      'nick':try_get_user_name(userid, nick or userid), 'userid':userid, 'status':status, 'activeTime':c_time()
    }


def add_user(username):
    userid = uuid_str_int31()
    users[userid] = username
    return userid

def try_get_user_name(userid, username):
  if userid not in users:
    users[userid] = username
  return users[userid]

def mk_room_info_for_user(roomId, userId=None, nick=None, json = True):
    srcRoomInfo = rooms[roomId]
    if userId and not srcRoomInfo["users"].get(userId):
        srcRoomInfo["users"][userId] = mk_user_info_for_room(userId, nick)
    if srcRoomInfo["hostIp"]:
        roomSecret = srcRoomInfo["roomSecret"]
        if globalparams["encryption"] and userId and not srcRoomInfo["users"][userId].get("private"):
            encKey = base64.b64encode(sha256(bytes(roomSecret + str(userId), encoding="latin-1")).digest()).decode("latin-1", "ignore")
            encUserId = bytes(str(userId),encoding="latin-1")
            srcRoomInfo["users"][userId]["private"] = {
              "authKey": base64.b64encode(
                  hmac.new(bytes(roomSecret, encoding="latin-1"), msg=encUserId, digestmod=sha256).digest()
               ).decode("latin-1", "ignore"),
              "encKey": encKey
            }
    roomInfo = deepcopy(srcRoomInfo)
    if roomInfo["hostIp"]:
        roomInfo["hostIp"] = get_host_ip()
    for uid, userInfo in roomInfo["users"].items():
        roomInfo.update(userInfo.get("public", {}))
        if uid == userId:
            roomInfo.update(userInfo.get("private", {}))
        if "private" in userInfo:
           del userInfo["private"]
    del roomInfo["roomSecret"]
    return jsonify(roomInfo) if json else roomInfo


def safeJson(s, default = None):
    try:
        print("params", json.loads(s))
        return json.loads(s)
    except Exception as e:
        return default

app = Flask(__name__)

@app.route('/')
def home():
    if "set_encryption" in request.args:
        globalparams["encryption"] = request.args["set_encryption"]=="true"
        return redirect(url_for("home"))

    encryption_text = "Set encryption <b>OFF</b>" if globalparams["encryption"] else "Set encryption <b>ON</b>"
    set_enc_to = "false" if globalparams["encryption"] else "true"
    return render_template_string(f"""
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="utf-8">
        <script>
          function refreshPage() {{
            // Send a hidden GET request with the "update_state" parameter
            fetch(window.location.pathname + "?set_encryption={set_enc_to}")
              .then(() => {{
                location.reload(); // Refresh the page after state update
              }});
         }}
        </script>
        <title>Home</title>
    </head>
    <body>
        <h1>Welcome to Outer Space Master Server Sample!</h1>
        <b>This is an admin\debug panel</b>
        <p>Server Ip is <b>{get_master_server_ip()}</b>, use it in master server url in client</p>
        <p><b>ATTENTION!</b><br/>Check that your master-server is available from clients by its url. Check that it's UDP ports in {globalparams["portrange_base"]}-{globalparams["portrange_base"]+globalparams["portrange_num"]} are also available</p>
        <p>Check that server can launch host process for session</p>
        <p>Game server dir: <b>{get_game_server_path()}</b></p>
        <p>Game server executable path: <b>{get_game_server_exe()}</b></p>
        <p>Traffic Encryption: <b>{globalparams["encryption"]}</b></p>
        <br/>
        <p><b>ATTENTION!</b><br/>This is just sample. No real scalability or security, not even authorization exists.
          It can be rather easily be changed to handle some noticeable amount of sessions and users, but it is done for only educational purpose. Feel free to use this code</b></p>
        <br/>
        <br/>
        DEBUG & ADMIN only links: <a href="/create_room">Create Room</a> | | <a href="/rooms_list">Rooms List</a> | <a href="/destroy_room">Destroy Room</a><br/>
         <button onclick="refreshPage()">{encryption_text}</button>
    </body>
    </html>
    """)

@app.route('/login', methods=['POST'])
def login():
    username = request.form.get('username') or generate_username()
    userid = add_user(username)
    payload = {
        'username': username,
        'userid': userid,
    }
    return jsonify(payload), 200


@app.route('/rename_user', methods=['POST'])
def rename_user():
    userid = request.form['userid']
    if userid not in users:
        return "USER DO NOT EXISTS, LOGIN FIRST", 409
    username = request.form['username']
    users[userid] = username
    return "SUCCESS", 200


@app.route('/create_room', methods=['GET', 'POST'])
def create_room():
    if request.method == 'POST':
        print("request to create room", request.form)

        roomId = str(uuid.uuid4())
        roomSecret = str(uuid.uuid4())
        owner_uid = request.form['userid']
        creater_nick = try_get_user_name(owner_uid, request.form.get('nick', 'unknown'))
        scene = request.form['scene']
        sessionId = uuid_str_int32() #uuid.uuid4() is str, but code should have int
        params = safeJson(request.form.get('params')) or {}
        params['maxPlayers'] = min(params.get('maxPlayers', 8), 8)
        hostIp = request.form.get('hostIp') or None
        hostPort = request.form.get('hostPort') or None
        timeCreated = datetime.datetime.utcnow().timestamp() #UTC time
        roomInfo = {'roomId':roomId, 'creater_nick':creater_nick,
            'scene':scene, 'sessionId':sessionId, 'hostIp':hostIp, 'lastTimeUsed':c_time(),
            'users':{ owner_uid: mk_user_info_for_room(owner_uid, creater_nick) },
            'roomSecret':roomSecret, 'timeCreated':timeCreated, 'params':params, 'owner_uid':owner_uid, 'hostPort':hostPort
          }
        rooms[roomId] = roomInfo
        roomsList.append(roomInfo)
        print("roomInfo", roomInfo)
        return mk_room_info_for_user(roomId, owner_uid, creater_nick), 200
    return render_template_string(f"""
    <!DOCTYPE html>
    <html>
    <head>
        <title>Create Room</title>
    </head>
    <body>
        <h1>Create Room</h1>
        <form method="post">
            creator userid: <input type="text" name="userid"><br>
            creator nick: <input type="text" name="nick"><br>
            scene: <input type="text" name="scene"><br>
            sessionId: <input type="text" name="sessionId" value=""><br>
            hostIp: <input type="text" name="serverIp" value="127.0.0.1"><br>
            hostPort: <input type="text" name="serverIp" value="20010"><br>
            params: <input type="text" name="params" value=""><br>
            <input type="submit" value="CreateRoom">
        </form>
    </body>
    </html>
    """)

@app.route('/join_room', methods=['POST'])
def join_room():
    roomId = request.form['roomId']
    userid = request.form['userid']
    nick = request.form.get('nick', userid)
    if roomId in rooms:
        roomInfo = rooms[roomId]
        roomInfo['users'].update({ userid:mk_user_info_for_room(userid, nick) })
        roomInfo['lastTimeUsed'] = c_time()
        return mk_room_info_for_user(roomId, userid, nick), 200
    else:
        return 'ROOM DOESNT EXIST', 409

@app.route('/set_status_in_room', methods=['POST'])
def set_status_in_room():
    roomId = request.form['roomId']
    userid = request.form['userid']
    status = request.form['status']
    nick = request.form.get('nick', userid)
    if roomId in rooms:
        roomInfo = rooms[roomId]
        roomInfo['users'][userid] = mk_user_info_for_room(userid, nick, status)
        roomInfo['lastTimeUsed'] = c_time()
        return mk_room_info_for_user(roomId, userid, nick, status), 200
    else:
        return jsonify({'status':'ROOM DOESNT EXIST'}), 409

def findRoomIdx(lst, key, uid):
    for i, dic in enumerate(lst):
        if dic[key] == uid:
            return i
    return None

def destroyRoom(roomId):
    if roomId not in rooms:
        return
    idx = findRoomIdx(roomsList, "roomId", roomId)
    if idx is not None:
        roomsList.pop(idx)
    del rooms[roomId]

def periodic_task():
    while True:
        # Task that runs every 10 seconds
        ctime = c_time()
        print("Periodic cleaner task running...", ctime)
        roomsToDelete = []
        for room in roomsList:
            print(room)
            #print(room['roomId'], room['lastTimeUsed'] +30 > ctime, room['lastTimeUsed'], ctime)
            room['users'] = {
              userid:user for userid, user in room['users'].items() if (user['activeTime'] + 5) > ctime
            } #10 seconds to kick non active users
            if len(room['users']) == 0 or \
              (room['lastTimeUsed'] + 30) <= ctime or \
              (room.get('timeSessionStarted', ctime) + 5) <= ctime or \
              (room.get('timeCreated', ctime) + 3600) <= ctime:
               #30 seconds for non active rooms, 5 for rooms with started sessions
                roomsToDelete.append(room['roomId'])
        for roomId in roomsToDelete:
            destroyRoom(roomId)
        ports_to_free = []
        for port, process in hosts_processes.items():
            if process.poll() is None: #still running
                continue
            ports_to_free.append(port)
        for port in ports_to_free:
            if port in hosts_processes:
                del hosts_processes[port]
        time.sleep(5)


@app.route('/destroy_room', methods=['POST'])
def destroy_room():
    roomId = request.form['roomId']
    roomSecret = request.form['roomSecret']
    if roomId not in rooms:
        return 'SUCCESS', 200
    elif roomSecret == rooms[roomId]['roomSecret']:
        destroyRoom(roomId)
    else:
        return 'Secret Key is incorret', 409

def transferOwnerShip(roomId):
  roomInfo = rooms[roomId]
  next_owner = next(iter(roomInfo["users"]))
  roomInfo['owner_uid'] = next_owner.get("userId", 0)

@app.route('/leave_room', methods=['POST'])
def leave_room():
    roomId = request.form['roomId']
    userid = request.form['userid']
    if roomId in rooms:
        roomInfo = rooms[roomId]
        if userid in roomInfo['users']:
          del roomInfo['users'][userid]
          if len(roomInfo['users'])==0:
              destroyRoom(roomId)
          else:
            roomInfo['lastTimeUsed'] = c_time()
            if roomInfo['owner_uid'] == userid:
                transferOwnerShip(roomId)
        return jsonify({'status':'SUCCESS'}), 200
    else:
        return jsonify({'status':'ROOM DOESNT EXIST'}), 409

class pushd:
    def __init__(self, path):
        self.olddir = os.getcwd()
        if path != '':
            os.chdir(os.path.normpath(path))
    def __enter__(self):
        pass
    def __exit__(self, type, value, traceback):
        os.chdir(self.olddir)

def start_host_processing(roomInfo):
    scene = roomInfo["scene"]
    secret = roomInfo["roomSecret"]
    hostIp = roomInfo["hostIp"]
    hostPort = roomInfo["hostPort"]
    params = roomInfo['params']
    game_server_path = get_game_server_path()
    exe = get_game_server_exe()
    inviteData = {
      "mode_info":{},
      "roomId":roomInfo["roomId"],
      "members":[
#        {"userId":12, "name":"foo"}
      ]
    }
    for userId, userInfo  in roomInfo["users"].items():
      memberInfo = {}
      memberInfo.update(userInfo)
      memberInfo.update({"userId":userId, "name":userInfo["nick"]})
      inviteData["members"].append(memberInfo)

    listenCmd = "--listen" if not hostIp else f"-listen:{hostIp}:{hostPort}"
    cmd = [exe, f'{listenCmd}', f'-scene:{scene}', '--no-sound', '--das-no-debugger', '-nostatsd',
            '-config:debug/profiler:t=off',
            f'-config:sessionLaunchParams/sessionMaxTimeLimit:r={params["sessionMaxTimeLimit"]}',
            f'-session_id:{roomInfo["sessionId"]}',
            '-invite_data=%s' % json_b64(inviteData),
            f'-config:sessionLaunchParams/maxPlayers:i={params["maxPlayers"]}'
          ]
    if globalparams["encryption"]:
        cmd.append(f'-room_secret:{secret}')
    else:
        cmd.extend(['-nonetenc', '-nopeerauth'])
    print("starting dedicated executable in:", game_server_path, ", scene:", scene, cmd)
    max_timeout = 5
    time_passed = 0
    step = 0.1
    with pushd(game_server_path):
        process = subprocess.Popen(cmd, shell=False, stderr=subprocess.STDOUT)
        hosts_processes[hostPort] = process
    # Wait for the process to start
    # You can check if the process has started by polling it and ensuring it's still running
    while True:
        # poll() returns None if the process is still running
        if process.poll() is None:
            # Process has started but is still running
            print("Process has started successfully.")
            break
        elif time_passed > max_timeout+step:
            process.terminate()
            break
        else:
            # Process hasn't started yet, wait a bit and check again
            time.sleep(step)
            time_passed += step

@app.route('/start_session', methods=['POST'])
def start_session():
    roomId = request.form['roomId']
    userid = request.form['userid']
    nick = request.form.get('nick')
    if roomId in rooms:
        roomInfo = rooms[roomId]
        if userid not in roomInfo['users']:
            print('USER NOT IN ROOM')
            return jsonify({'status':'USER NOT IN ROOM'}), 409
        print("Starting Session", roomId, userid)
        roomInfo["hostIp"] = get_host_ip()
        roomInfo["hostPort"] = get_free_udp_socket()
        start_host_processing(roomInfo)
        roomInfo["timeSessionStarted"] = c_time()
        return mk_room_info_for_user(roomId, userid, nick), 200
    else:
        return jsonify({'status':'ROOM DOESNT EXIST'}), 409


def rooms_to_html():
  res = ["<br/>"]
  for roominfo in roomsList:
    res.append(f"{roominfo['roomId']}: creater_nick:{roominfo['creater_nick']}, scene:{roominfo['scene']}, sessionId: {roominfo['sessionId']}, hostIp:{roominfo['hostIp']}, users:{','.join(roominfo['users'].keys())}")
  return "<br/><br/>".join(res)

@app.route('/rooms_list', methods=['GET', 'POST'])
def rooms_list():
    if request.method == 'POST':
        userid = request.form.get("userid")
        nick = request.form.get("nick")
        return jsonify([mk_room_info_for_user(room["roomId"], userid, nick, json=False) for room in roomsList]), 200
    return render_template_string(f"""
    <!DOCTYPE html>
    <html>
    <head>
        <title>Rooms List</title>
    </head>
    <body>
        <h1>Rooms List</h1>
        {rooms_to_html()}
    </body>
    </html>
    """)


@app.route('/room_info', methods=['GET', 'POST'])
def room_info():
    if request.method == 'POST':
        roomId = request.form['roomId']
        userid = request.form.get('userid')
        nick = request.form.get('nick')
        if roomId in rooms:
            roomInfo = rooms[roomId]
            roomInfo['lastTimeUsed'] = c_time()
            if userid:
                curUserRoomInfo = roomInfo["users"].get(userid)
                if curUserRoomInfo:
                      roomInfo["users"][userid] = mk_user_info_for_room(userid, nick, curUserRoomInfo["status"])
            return mk_room_info_for_user(roomId, userid, nick), 200
        else:
           return jsonify({'status': "ROOM DOESNT EXIST"}), 409
    return render_template_string(f"""
    <!DOCTYPE html>
    <html>
    <head>
        <title>Room Info</title>
    </head>
        <form method="post">
            roomId: <input type="text" name="roomId"><br>
            <input type="submit" value="GetRoomInfo">
        </form>
    </html>
    """)

if __name__ == '__main__':
    # Start the periodic task in a separate thread
    parser = argparse.ArgumentParser(description='DNG Master Server Sample.')
    parser.add_argument('-exe_path', type=str, default=globalparams["exe_path"], help='Path to exe in game server directory')
    parser.add_argument('-encryption', type=bool, default=globalparams["encryption"], help='Set encryption of game traffic')
    parser.add_argument('-game_server_dir', type=str, default=globalparams["game_server_dir"], help='Path to server directory')
    parser.add_argument('-external_fixed_host_ip', type=str, default=globalparams["external_fixed_host_ip"], help='Use fixed external ip for host')
    parser.add_argument('-portrange_base', type=int, default=globalparams["portrange_base"], help='Start UDP port range')
    parser.add_argument('-portrange_num', type=int, default=globalparams["portrange_num"], help='Number of UDP that can be used (limits amount of simultaneously running hosts)')
    parser.add_argument('-listen-ip', type=str, default="0.0.0.0", help='IP on which master server will be listen')
    parser.add_argument('-listen-port', type=int, default=None, help='port on which master server will be listen')
    parser.add_argument('-debug', default=False, help='Debug web server mode', action="store_true")
    args = parser.parse_args()
    print(args)
    globalparams["portrange_num"] = args.portrange_num
    globalparams["portrange_base"] = args.portrange_base
    globalparams["encryption"] = args.encryption
    globalparams["exe_path"] = args.exe_path
    globalparams["external_fixed_host_ip"] = args.external_fixed_host_ip
    globalparams["game_server_dir"] = args.game_server_dir
    print(globalparams)
    task_thread = threading.Thread(target=periodic_task)
    task_thread.daemon = True  # Ensures the thread will exit when the main program does
    task_thread.start()
    ip_for_browser = args.listen_ip if args.listen_ip!="0.0.0.0" else "127.0.0.1"
    if not args.debug:
      threading.Timer(1.0, lambda: webbrowser.open(f"http://{ip_for_browser}:{args.listen_port or 5000}")).start()
    app.run(debug=args.debug, host=args.listen_ip, port = args.listen_port)
