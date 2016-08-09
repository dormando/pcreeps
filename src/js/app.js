var Clay = require('pebble-clay');
var clayConfig = require('./config');

var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// light brown: 232
// white: 255
// red: 240
// bluemoon: 199
// brightblue: 239
// green: 220

var gameColors = {
    plain: 213,
    road: 234,
    wall: 192,
    constructedWall: 192,
    constructionSite: 232,
    extension: 252,
    spawn: 252,
    tower: 252,
    link: 252,
    storage: 252,
    container: 252,
    swamp: 216,
    source: 252,
    creep: 199,
    controller: 255,
    mineral: 255
};

var xToken;
var prefix = 'https://screeps.com/api/';

/* Convenience routine for making authorized requests to screeps. Needs to
 * replace X-Token if a new one was supplied in a response.
 */
var xhrRequest = function (url, type, callback, data) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    var newToken = this.getResponseHeader('X-Token');
    if (newToken) {
        xToken = newToken;
    }
    //console.log("response text:", this.responseText);
    callback(JSON.parse(this.responseText));
  };
  xhr.open(type, url);
  if (type == "POST") {
    xhr.setRequestHeader('Content-Type', 'application/json; charset=utf-8');
  }
  if (xToken) {
    xhr.setRequestHeader('X-Username', xToken);
    xhr.setRequestHeader('X-Token', xToken);
  }
  if (data) {
      xhr.send(JSON.stringify(data));
  } else {
      xhr.send();
  }
};

function getPixel(item) {
    var color;
    if (gameColors[item.type]) {
        color = gameColors[item.type];
    } else {
        // FIXME: Just for your scructures. Red for non-yours.
        console.log("no color for type:", item.type);
        color = 220;
    }

    // Represent empty energy buildings.
    switch(item.type) {
        case "extension":
        case "spawn":
        case "tower":
        case "storage":
        case "link":
            if (item.energy == 0) color = 232;
            break;
    }

    return color;
}

// FIXME: for some reason fetching from localstorage then sending to
// appmessage is causing the emulator to wig out.
function getRoomTerrain(room, terrain, callback) {
    //var terrain = localStorage.getItem('terrain' + room);
    if (terrain) {
        //var terrain = JSON.parse(terrain);
        //console.log("REUSING TERRAIN", room, terrain.length);
        callback(terrain);
    } else {
        //console.log("FETCHING TERRAIN", room);
        xhrRequest(prefix + 'game/room-terrain?room=' + room + '&encoded=1', 'GET',
            function(ures) {
                if (ures.ok) {
                    //console.log("got OK terrain response");
                } else {
                    console.log("got BAD terrain response", JSON.stringify(ures));
                    return;
                }

                var encoded = ures.terrain[0].terrain.split('');
                var mapped = [];
                encoded.forEach(function(toMap) {
                    var color;
                    switch (Number(toMap)) {
                        case 0:
                            color = 'plain';
                            break;
                        case 1:
                        case 3:
                            color = 'wall';
                            break;
                        case 2:
                            color = 'swamp';
                            break;
                    }
                    mapped.push(gameColors[color]);
                });
                //console.log("mapped terrain size:", mapped.length);

                //localStorage.setItem('terrain' + room, JSON.stringify(mapped));

                getRoomTerrain(room, mapped, callback);
            }
        );
    }
}

function sendFrame(frames, index) {
    Pebble.sendAppMessage({'FRAME': frames[index]},
        function(e) {
            //console.log("sent frame");
            index++;
            if (index < frames.length) {
                sendFrame(frames, index);
            }
        },
        function(e) {
            console.log("failed to send frame");
        }
    );
}

function sendFrames(frames) {
    Pebble.sendAppMessage({'TERRAIN': frames.start},
        function(e) {
            //console.log("sent first frame");
            var index = 0;
            sendFrame(frames.frames, index);
        },
        function(e) {
            console.log("failed to send first frame");
        }
    );
}

/* Extremely simple method of coming up with new frames. Not actually sure
 * what the performance timing is for the phone. Would be potentially more
 * efficient to track dirty pixels frame by frame and patching via the original
 * image.
 */
function diffImage(index, oldImg, newImg) {
    var diff = [index];
    for (var y = 0; y < 50; y++) {
        for (var x = 0; x < 50; x++) {
            var pix = x + (y * 50);
            if (oldImg[pix] !== newImg[pix]) {
                //console.log("differing pixel:", oldImg[pix], newImg[pix]);
                diff.push(newImg[pix], x, y);
            }
        }
    }
    return diff;
}

// TODO: Find out what kind of events happen from object death/etc.
function makeHistoryFrames(terrain, hist) {
    var curTick = hist.base;
    var view = hist.ticks[curTick];
    var uniques = {};
    var skips = { rampart: true,
                  energy: true,
                  extractor: true
    };
    var writeImage = function(todo) {
        var map = terrain.slice(0);
        var id;
        for (id in todo) {
            var item = view[id];
            if (skips[item.type]) {
                continue;
            }
            if (!uniques[item.type]) {
                uniques[item.type] = item;
            }
            map[item.x + (item.y * 50)] = getPixel(item);
        }
        return map;
    };

    // We send a baseline image with the original "view" of game objects
    // pre-patched in. The first frame is the biggest diff
    // (roads/buildings/etc) so we compress two down to one. Then the rest are
    // the per-tick diffs from there.
    var startImage = writeImage(view);
    var lastImage = startImage;
    var frames = [];
    curTick++;
    var index = 0;
    // Generate the per-tick frame diffs, which directly patch pixels.
    while (hist.ticks[curTick]) {
        var nextTick = hist.ticks[curTick];
        for (var id in nextTick) {
            if (view[id]) {
                for (var prop in nextTick[id]) {
                    //console.log("new prop for", id, prop, nextTick[id][prop]);
                    view[id][prop] = nextTick[id][prop];
                }
            } else {
                view[id] = nextTick[id];
            }
        }
        var curImage = writeImage(view);
        frames.push(diffImage(index++, lastImage, curImage));
        lastImage = curImage;
        curTick++;
        //console.log("next index: ", index, curTick);
    }
    //console.log("FINAL INDEX:", index);

    // Useful when figuring out how to display a new type.
    //console.log("UNIQUES:", JSON.stringify(uniques, null, 4));

    // now we have startImage, and a number of frames. Send each to the watch.
    return { start: startImage, frames: frames };
}

function sendRoomHistory(room) {
    getRoomTerrain(room, undefined, function(terrain) {
        xhrRequest(prefix + 'game/time', 'GET',
            function(res) {
                if (!res) return;
                if (!res.ok) {
                    console.log('Got BAD response from game/time');
                    return;
                }

                // history isn't always available immediately.
                var time = res.time - 120;
                time -= time % 20;
                xhrRequest('https://screeps.com/room-history/' + room + '/' + time + '.json', 'GET',
                    function(hist) {
                        //console.log("room history:", JSON.stringify(hist));
                        var frames = makeHistoryFrames(terrain, hist);
                        //console.log("made frames from room history, sending.");
                        sendFrames(frames);
                    }
                );
            }
        );
    });
}

// TODO: local xToken from localstorage, avoid hammering auth/signin.
/*function logIn() {

}*/

function sendRoomByIndex(index) {
    xhrRequest(prefix + 'user/overview?interval=180&statName=energyControl', 'GET',
        function(ures) {
            if (!ures.rooms[index]) {
                index = 0;
            }
            var room = ures.rooms[index];
            Pebble.sendAppMessage({'ROOMNAME': room},
                    function(e) {
                        //console.log("sent roomname");
                        Pebble.sendAppMessage({'ROOMCOUNT': ures.rooms.length},
                            function(e) {
                                sendRoomHistory(room);
                            },
                            function(e) {
                                console.log("failed to send room count");
                            }
                        );
                    },
                    function(e) {
                        console.log("failed to send roomname");
                    }
            );
        });
}

// Listen for when the watchface is opened
Pebble.addEventListener('ready', function(e) {
    var email = localStorage.getItem('email');
    var password = localStorage.getItem('password');

    // FIXME: these undefined checks aren't working for some reason.
    if (email !== undefined && password !== undefined) {
        console.log("got email/pass from localstorage", email, password);
        xhrRequest(prefix + 'auth/signin', 'POST',
            function(res) {
                if (res) {
                    xToken = res.token;
                    sendRoomByIndex(0);
                }
            },
            { 'email': email,
              'password': password
            }
        );
    } else {
        // have 32 bytes to work with for room name.
        Pebble.sendAppMessage({'ROOMNAME': 'please run setup'},
            function(e) {
                console.log("asked user to run setup");
            },
            function(e) {
                console.log("failed to ask user to run setup");
            }
        );
    }
});

// Listen for when an AppMessage is received
Pebble.addEventListener('appmessage',
  function(e) {
    var dict = e.payload;
    //console.log('AppMessage received!', JSON.stringify(dict));

    if (dict['SWITCH'] !== undefined) {
        sendRoomByIndex(dict['SWITCH']);
    }
  }
);

// For the configurator. Note clay.getSettings is only useful if passing these
// options directly into the watch.
Pebble.addEventListener('webviewclosed', function(e) {
    if (e && !e.response) {
        return;
    }

    // want to do this if we need to transfer these to the watch.
    //var dict = clay.getSettings(e.response);

    var dict = JSON.parse(e.response);
    //console.log("got config response:", e.response);
    localStorage.setItem('email', dict.email.value);
    localStorage.setItem('password', dict.password.value);
});

// Not actually sure if this is necessary but I can't test configurations in
// the emulator right now.
Pebble.addEventListener('showConfiguration', function(e) {
    Pebble.openURL(clay.generateUrl());
});
