// Import Clay library
var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

// Keep track of the current TfL API key
var tflApiKey = "";

// Function to load settings
function loadSettings() {
    var storedKey = localStorage.getItem('TFL_API_KEY');
    if (storedKey) {
        tflApiKey = storedKey;
    }
}

// Function to handle AppMessage requests from C side
Pebble.addEventListener('ready', function(e) {
    console.log('PebbleKit JS ready!');
    loadSettings();
    // Fetch stops and arrivals on launch
    fetchLocationAndArrivals();
});

// Watch received an AppMessage
Pebble.addEventListener('appmessage', function(e) {
    console.log('Received message from watch: ' + JSON.stringify(e.payload));
    var payload = e.payload;
    if (payload.TFL_API_KEY !== undefined) {
        tflApiKey = payload.TFL_API_KEY;
        localStorage.setItem('TFL_API_KEY', tflApiKey);
        console.log('Updated TfL API Key from watch: ' + tflApiKey);
    }
    if (payload.REQ_UPDATE !== undefined) {
        console.log('Watch requested update');
        fetchLocationAndArrivals();
    }
});

// Handle webview closed (settings saved)
Pebble.addEventListener('webviewclosed', function(e) {
    console.log('Webview closed');
    loadSettings();
    // Fetch immediately on config close
    fetchLocationAndArrivals();
});

// Send status message to C side
function sendStatus(message) {
    Pebble.sendAppMessage({
        "STATUS_MSG": message
    }, function() {
        console.log('Sent status to watch: ' + message);
    }, function(e) {
        console.log('Failed to send status to watch: ' + JSON.stringify(e));
    });
}

// Global Message Queue to stream updates reliably
var messageQueue = [];
var isSendingQueue = false;

function queueMessage(dict) {
    messageQueue.push(dict);
    if (!isSendingQueue) {
        isSendingQueue = true;
        sendNextQueuedMessage();
    }
}

function sendNextQueuedMessage() {
    if (messageQueue.length === 0) {
        isSendingQueue = false;
        sendStatus("Done");
        return;
    }

    var dict = messageQueue[0];
    Pebble.sendAppMessage(dict, function() {
        console.log('Successfully sent message to watch: ' + JSON.stringify(dict));
        messageQueue.shift();
        setTimeout(sendNextQueuedMessage, 150); // 150ms delay between messages
    }, function(err) {
        console.log('Failed to send message: ' + JSON.stringify(err) + '. Retrying...');
        // Retry up to 3 times
        dict.retryCount = (dict.retryCount || 0) + 1;
        if (dict.retryCount < 3) {
            setTimeout(sendNextQueuedMessage, 1000);
        } else {
            messageQueue.shift();
            setTimeout(sendNextQueuedMessage, 150);
        }
    });
}

// Clear watch lists before streaming new update
function sendClean() {
    messageQueue = []; // Clear any pending sends
    isSendingQueue = false;
    Pebble.sendAppMessage({
        "DATA_CLEAN": 1
    }, function() {
        console.log('Sent clean command to watch');
    }, function(e) {
        console.log('Failed to send clean command: ' + JSON.stringify(e));
    });
}

// Fetch geolocation and then fetch TfL API
function fetchLocationAndArrivals() {
    sendStatus("Getting location...");
    navigator.geolocation.getCurrentPosition(
        function(position) {
            var lat = position.coords.latitude;
            var lon = position.coords.longitude;
            console.log('Got GPS: ' + lat + ', ' + lon);
            getTfLArrivals(lat, lon);
        },
        function(err) {
            console.log('GPS error: ' + JSON.stringify(err) + '. Using default coords (London Central)');
            // Fallback to central London coordinates if GPS fails
            getTfLArrivals(51.5074, -0.1278);
        },
        { enableHighAccuracy: true, timeout: 15000, maximumAge: 10000 }
    );
}

// Core TfL API logic
function getTfLArrivals(lat, lon) {
    sendStatus("Fetching stops...");
    
    // We want both Tube (NaptanMetroStation) and Bus (NaptanPublicBusCoachTram) stop types.
    var stopTypes = "NaptanMetroStation,NaptanPublicBusCoachTram";
    var radius = 600;
    var url = "https://api.tfl.gov.uk/StopPoint?lat=" + lat + "&lon=" + lon + 
              "&stopTypes=" + stopTypes + "&radius=" + radius + "&useStopPointHierarchy=true";
    
    if (tflApiKey && tflApiKey.trim().length > 0) {
        url += "&app_key=" + encodeURIComponent(tflApiKey.trim());
    }
    
    console.log('Requesting StopPoints: ' + url);
    
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url);
    xhr.onload = function() {
        if (xhr.status === 200) {
            try {
                var response = JSON.parse(xhr.responseText);
                var stopPoints = response.stopPoints || [];
                if (stopPoints.length === 0) {
                    sendStatus("No nearby stops found");
                    return;
                }
                
                // Group stop points by Mode (Tube / Bus) and find closest ones
                var tubeStops = [];
                var busStops = [];
                
                for (var i = 0; i < stopPoints.length; i++) {
                    var sp = stopPoints[i];
                    var isTube = false;
                    var isBus = false;
                    
                    if (sp.modes) {
                        if (sp.modes.indexOf('tube') > -1 || sp.modes.indexOf('underground') > -1 || sp.modes.indexOf('dlr') > -1 || sp.modes.indexOf('overground') > -1 || sp.modes.indexOf('elizabeth-line') > -1) {
                            isTube = true;
                        }
                        if (sp.modes.indexOf('bus') > -1) {
                            isBus = true;
                        }
                    }
                    
                    if (sp.stopType === 'NaptanMetroStation') {
                        isTube = true;
                    } else if (sp.stopType === 'NaptanPublicBusCoachTram' || sp.stopType === 'NaptanOnstreetBusCoachStopPair') {
                        isBus = true;
                    }
                    
                    var distance = sp.distance || 9999;
                    var stopInfo = {
                        id: sp.id,
                        name: sp.commonName,
                        distance: distance,
                        isTube: isTube,
                        isBus: isBus
                    };
                    
                    if (isTube) tubeStops.push(stopInfo);
                    if (isBus) busStops.push(stopInfo);
                }
                
                // Sort by distance (closest first)
                tubeStops.sort(function(a, b) { return a.distance - b.distance; });
                busStops.sort(function(a, b) { return a.distance - b.distance; });
                
                // Select nearest tube stations and bus stops
                var selectedStops = [];
                
                if (tubeStops.length > 0) {
                    selectedStops.push(tubeStops[0]);
                    console.log('Selected Tube Stop: ' + tubeStops[0].name + ' (' + tubeStops[0].id + ')');
                }
                for (var b = 0; b < Math.min(busStops.length, 2); b++) {
                    selectedStops.push(busStops[b]);
                    console.log('Selected Bus Stop: ' + busStops[b].name + ' (' + busStops[b].id + ')');
                }
                
                if (selectedStops.length === 0) {
                    sendStatus("No stops found");
                    return;
                }
                
                fetchArrivalsForStops(selectedStops);
                
            } catch (ex) {
                console.log('Error parsing StopPoints JSON: ' + ex);
                sendStatus("Parse error (stops)");
            }
        } else {
            console.log('StopPoints API error: ' + xhr.status + ' - ' + xhr.statusText);
            sendStatus("StopPoint API Error");
        }
    };
    xhr.onerror = function() {
        sendStatus("Network Error");
    };
    xhr.send();
}

// Fetch arrivals for multiple selected stops, sort them, and send to C
function fetchArrivalsForStops(stops) {
    sendStatus("Fetching arrivals...");
    var rawArrivals = [];
    var pendingRequests = stops.length;
    
    if (pendingRequests === 0) {
        sendStatus("No selected stops");
        return;
    }
    
    stops.forEach(function(stop) {
        var url = "https://api.tfl.gov.uk/StopPoint/" + stop.id + "/Arrivals";
        if (tflApiKey && tflApiKey.trim().length > 0) {
            url += "?app_key=" + encodeURIComponent(tflApiKey.trim());
        }
        
        console.log('Requesting arrivals for ' + stop.name + ': ' + url);
        
        var xhr = new XMLHttpRequest();
        xhr.open('GET', url);
        xhr.onload = function() {
            pendingRequests--;
            if (xhr.status === 200) {
                try {
                    var arrivals = JSON.parse(xhr.responseText) || [];
                    console.log('Got ' + arrivals.length + ' arrivals for stop ' + stop.name);
                    
                    arrivals.forEach(function(arr) {
                        rawArrivals.push({
                            stop: stop,
                            route: arr.lineName || "?",
                            direction: arr.towards || arr.destinationName || "Unknown",
                            etaSeconds: arr.timeToStation || 0,
                            platformName: arr.platformName || "",
                            towards: arr.towards || ""
                        });
                    });
                } catch (e) {
                    console.log('Error parsing arrivals for ' + stop.name + ': ' + e);
                }
            } else {
                console.log('Arrivals API error for ' + stop.name + ': ' + xhr.status);
            }
            
            // Once all requests are complete, process and send
            if (pendingRequests === 0) {
                processAndSendMultiScreen(rawArrivals);
            }
        };
        xhr.onerror = function() {
            pendingRequests--;
            console.log('Network error fetching arrivals for ' + stop.name);
            if (pendingRequests === 0) {
                processAndSendMultiScreen(rawArrivals);
            }
        };
        xhr.send();
    });
}

function processAndSendMultiScreen(arrivals) {
    if (arrivals.length === 0) {
        sendStatus("No upcoming arrivals");
        return;
    }

    // Group arrivals by Stop + Direction/Platform separately
    var stopsMap = {};
    var stopsList = [];
    var tempArrivals = [];

    arrivals.forEach(function(arr) {
        var stopName = arr.stop.name;
        // Clean up names for Pebble's small screens
        stopName = stopName.replace(" Underground Station", "");
        stopName = stopName.replace(" DLR Station", "");
        stopName = stopName.replace(" Overground Station", "");
        stopName = stopName.replace(" Coach Station", "");
        stopName = stopName.replace(" Rail Station", "");
        
        // Use platform name (like "Northbound - Platform 1" or "Stop L") or towards
        var stopDetail = arr.platformName || arr.towards || "";
        if (!stopDetail && arr.stop.isBus) {
            stopDetail = "Bus Stop";
        } else if (!stopDetail) {
            stopDetail = "Departures";
        }

        // Create a unique key for grouping Stop + Direction/Platform
        var key = arr.stop.id + "_" + stopDetail;
        if (stopsMap[key] === undefined) {
            stopsMap[key] = {
                tempId: stopsList.length,
                name: stopName,
                detail: stopDetail,
                distance: arr.stop.distance,
                type: arr.stop.isTube ? 0 : 1
            };
            stopsList.push(stopsMap[key]);
        }

        tempArrivals.push({
            tempStopId: stopsMap[key].tempId,
            route: arr.route,
            direction: arr.direction,
            eta: Math.round(arr.etaSeconds / 60),
            type: arr.stop.isTube ? 0 : 1
        });
    });

    // Sort the stops by proximity (closest first) so the closest stop is at the top
    stopsList.sort(function(a, b) {
        return a.distance - b.distance;
    });

    // Limit to max 6 stops on the watch screen for best performance
    stopsList = stopsList.slice(0, 6);

    // Map old tempId to new sorted id, and prepare an allowed check
    var idMap = {};
    var allowedStopIds = {};
    stopsList.forEach(function(stop, index) {
        idMap[stop.tempId] = index;
        stop.id = index;
        allowedStopIds[index] = true;
    });

    // Translate tempStopId to final sorted stopId and filter out excess stops
    var finalArrivals = [];
    tempArrivals.forEach(function(arr) {
        var finalId = idMap[arr.tempStopId];
        if (finalId !== undefined && allowedStopIds[finalId] === true) {
            arr.stopId = finalId;
            delete arr.tempStopId;
            finalArrivals.push(arr);
        }
    });

    // Sort finalArrivals by stopId, then by ETA (soonest first)
    finalArrivals.sort(function(a, b) {
        if (a.stopId !== b.stopId) {
            return a.stopId - b.stopId;
        }
        return a.eta - b.eta;
    });

    // Limit total arrivals to 15 to prevent memory overflow on the watch
    finalArrivals = finalArrivals.slice(0, 15);

    console.log('Prepared ' + stopsList.length + ' stops and ' + finalArrivals.length + ' arrivals. Streaming to watch...');

    // 1. Send DATA_CLEAN to start with a fresh slate on the watch
    sendClean();

    // 2. Queue all the stops to be sent
    stopsList.forEach(function(stop, index) {
        queueMessage({
            "STOP_INDEX": index,
            "STOP_COUNT": stopsList.length,
            "STOP_NAME": stop.name.substring(0, 31),
            "STOP_DETAIL": stop.detail.substring(0, 31),
            "STOP_TYPE": parseInt(stop.type)
        });
    });

    // 3. Queue all the arrivals to be sent
    finalArrivals.forEach(function(arr, index) {
        queueMessage({
            "DATA_INDEX": index,
            "DATA_COUNT": finalArrivals.length,
            "DATA_STOP_ID": parseInt(arr.stopId),
            "DATA_ROUTE": arr.route.substring(0, 15),
            "DATA_DIRECTION": arr.direction.substring(0, 31),
            "DATA_ETA": parseInt(arr.eta),
            "DATA_TYPE": parseInt(arr.type)
        });
    });

    // The queue will automatically start sending, transitioning s_status_text to "Done" upon completion!
}
