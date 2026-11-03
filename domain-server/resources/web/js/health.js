// "Check my world" on the overview: whether other people can reach this world, and if not, why and how to fix it.
// The server gives the facts (/api/health, plus the sign-in state from /settings.json); the wording and the fixes are
// here. These are the checks the server can make by itself. Whether it answers from outside your network needs
// something outside it to knock, which is shown as not checked yet.
$(document).ready(function() {
  var $card = $('#dr-health');
  if (!$card.length) {
    return;
  }

  var LAST_PASS_KEY = "darkride.healthLastPass";
  var lastResults = null;
  var lastHealth = null;

  var STATES = {
    ok: { label: "OK", order: 0 },
    info: { label: "Note", order: 1 },
    unknown: { label: "Not checked", order: 1 },
    warn: { label: "Look at this", order: 2 },
    fail: { label: "Stops visitors", order: 3 }
  };

  function serviceName(type) {
    var service = _.find(Services.list, function(service) { return service.type === type; });
    return service ? service.name : type;
  }

  // addresses only reachable inside a home or office network, or this computer
  function isPrivateAddress(ip) {
    return /^(10\.|127\.|192\.168\.|169\.254\.|172\.(1[6-9]|2\d|3[01])\.)/.test(ip) || ip === "::1" ||
      /^f[cd]/i.test(ip);
  }

  // 100.64.0.0/10: an internet provider sharing one address between customers (carrier-grade NAT)
  function isSharedProviderAddress(ip) {
    var match = /^100\.(\d+)\./.exec(ip);
    return !!match && Number(match[1]) >= 64 && Number(match[1]) <= 127;
  }

  function timeText(ms) {
    if (!ms) {
      return "never";
    }
    var seconds = Math.round((Date.now() - ms) / 1000);
    if (seconds < 90) {
      return seconds <= 1 ? "just now" : seconds + " seconds ago";
    }
    return Services.durationText(seconds) + " ago";
  }

  // the address a visitor types: the server's public one when it knows it, else one a service found
  function publicAddress(health) {
    if (health.network.public && health.network.public.ip) {
      return health.network.public.ip;
    }
    var found = _.find(health.services, function(service) {
      return service.running && service.public && service.public.ip && !isPrivateAddress(service.public.ip);
    });
    return found ? found.public.ip : null;
  }

  function checkServices(health) {
    var waiting = _.filter(health.services, function(service) { return !service.running; });
    var running = health.services.length - waiting.length;
    if (health.services.length === 0) {
      return {
        state: "fail", title: "Services running", summary: "None have started",
        why: "Visitors need the world's services (objects, avatars, voice and so on), and none has connected yet.",
        steps: [
          "Give it a minute after the server starts.",
          "Check the assignment client is running and points at this server: on Windows, from the server folder, " +
            "assignment-client.exe -n 6 --max 6 --min-listen-port 40110 -a 127.0.0.1 --server-port " +
            health.network.local_port + ". On Docker, look at the container's log."
        ]
      };
    }
    if (waiting.length > 0) {
      return {
        state: "fail", title: "Services running", summary: running + " of " + health.services.length,
        why: "Still waiting: " + _.map(waiting, function(service) { return serviceName(service.type); }).join(", ") +
          ". Visitors can't get in properly until every service is running.",
        steps: [
          "If the server has only just started, give it a minute.",
          "Otherwise, start more assignment clients: they need one for each service (-n 6 --max 6, or 7 with " +
            "vehicle physics).",
          "Check the assignment clients can reach this server: their -a address and --server-port " +
            health.network.local_port + "."
        ]
      };
    }
    return { state: "ok", title: "Services running", summary: running + " of " + health.services.length };
  }

  function checkPublicAddress(health) {
    var network = health.network;
    var title = "Public address";
    if (network.public && network.public.ip) {
      var ip = network.public.ip;
      if (isSharedProviderAddress(ip) || isPrivateAddress(ip)) {
        return {
          state: "fail", title: title, summary: ip + " is not a public address",
          why: "Your internet provider seems to share one address between several customers (carrier-grade NAT), " +
            "so nothing on the internet can start a connection to your server, and port forwarding can't fix it.",
          steps: [
            "Ask your provider for a public IPv4 address (some call it a static or dedicated IP).",
            "Or run the world on a hosted server (a VPS) instead.",
            "Or turn on full automatic networking under Settings > Networking, if your world uses an ICE server."
          ]
        };
      }
      if (network.public.port && network.public.port !== network.local_port) {
        return {
          state: "warn", title: title,
          summary: ip + ", but the router changes the port (" + network.local_port + " becomes " +
            network.public.port + ")",
          why: "Your router changes this server's port on the way out, which usually means nothing forwards the port " +
            "to it yet. Visitors are sent to port " + network.local_port + ".",
          steps: [
            "Forward UDP " + network.local_port + " on your router to this computer (" + network.local.ip + ").",
            "Or turn on full automatic networking under Settings > Networking, if your world uses an ICE server."
          ]
        };
      }
      return { state: "ok", title: title, summary: ip + ", port " + network.local_port };
    }
    if (health.world.registration === "none") {
      var seen = publicAddress(health);
      return {
        state: "info", title: title,
        summary: seen ? "Your services see " + seen : "Not looked up",
        why: "The server only looks up its public address when your world is registered with the directory (so it " +
          "can keep the directory's copy current). Visitors can still come in by address: hifi://<your public IP>:" +
          network.local_port + "."
      };
    }
    if (!network.stun_completed) {
      return {
        state: "fail", title: title, summary: "Couldn't find it",
        why: "The server asks a public service (stun.l.google.com, UDP 19302) for its address, and had no answer. " +
          "Outgoing UDP may be blocked, or the computer isn't online.",
        steps: [
          "Check this computer can reach the internet.",
          "Allow outgoing UDP in its firewall (and the network's, at work or college).",
          "Restart the server once that's fixed."
        ]
      };
    }
    return { state: "warn", title: title, summary: "Still looking it up", why: "Check again in a moment." };
  }

  function checkOutside(health) {
    var ip = publicAddress(health);
    return {
      state: "unknown", title: "Reachable from outside", summary: "Not checked yet",
      why: "A server can't tell by itself whether people outside your network reach it: testing your own public " +
        "address from inside is unreliable. A check from outside is coming. Until then, ask someone elsewhere to " +
        "visit your world.",
      steps: [
        "Ask a friend (not on your Wi-Fi) to go to " + (ip ? "hifi://" + ip + ":" + health.network.local_port :
          "hifi://<your public IP>:" + health.network.local_port) + ".",
        "If they can't get in, forward UDP " + health.network.local_port + " on your router to this computer (" +
          health.network.local.ip + "), and allow it in the firewall.",
        "On a hosted server (a VPS), also open it in the provider's own firewall, in their control panel."
      ]
    };
  }

  function checkServicePorts(health) {
    var ports = _.sortBy(_.compact(_.map(health.services, function(service) {
      return service.running && service.public ? service.public.port : null;
    })), _.identity);
    if (ports.length === 0) {
      return { state: "unknown", title: "Service ports", summary: "Not checked: no service is running" };
    }
    var list = "UDP " + ports.join(", ");
    // ports the system picked (ephemeral, 49152 and up) change on every restart, so they can't be forwarded
    var random = _.filter(ports, function(port) { return port >= 49152; });
    if (random.length > 0) {
      return {
        state: "warn", title: "Service ports", summary: list,
        why: "Some services have ports the system picked, which change every time they restart, so they can't be " +
          "forwarded. Visitors connect to each service directly, as well as to port " + health.network.local_port + ".",
        steps: [
          "Start the assignment clients with --min-listen-port 40110, so the services use 40110 and up.",
          "Then forward and open UDP 40110 to 40115 (40116 too with vehicle physics)."
        ]
      };
    }
    return {
      state: "info", title: "Service ports", summary: list,
      why: "Visitors connect to each of these directly, as well as to port " + health.network.local_port +
        ". Forward and open all of them (UDP), and never the admin pages' port.",
    };
  }

  function checkDirectory(health) {
    var directory = health.directory;
    var title = "Listed in Explore";
    if (health.world.registration === "none") {
      return {
        state: "info", title: title, summary: "Not registered",
        why: "Your world isn't registered with the directory (" + directory.metaverse_url + "), so it isn't in " +
          "Explore and people need its address to come in. That's fine for a private world.",
        steps: ["To list it, connect your Darkride account and set up your world's ID under Settings > Metaverse."]
      };
    }
    if (!directory.heartbeats_running) {
      return {
        state: "fail", title: title, summary: "Not talking to the directory",
        why: "The server stopped telling the directory about itself" +
          (directory.last_error ? " after an error: " + directory.last_error : "") + ".",
        steps: [
          "Check the world ID and the account under Settings > Metaverse.",
          "Restart the server to try again."
        ]
      };
    }
    if (directory.last_failed && directory.last_failed > directory.last_ok) {
      var signIn = /HTTP 40[13]/.test(directory.last_error);
      return {
        state: "fail", title: title, summary: "The directory said no " + timeText(directory.last_failed),
        why: "The last update to the directory failed: " + directory.last_error + ".",
        steps: signIn ? [
          "The directory didn't accept this world's sign-in. Connect your account again under Settings > Metaverse.",
          "Check the world ID there belongs to your account."
        ] : [
          "Check this computer can reach " + directory.metaverse_url + ".",
          "If it keeps failing, the directory may be down: try again later."
        ]
      };
    }
    if (!directory.last_ok) {
      return { state: "warn", title: title, summary: "No update yet", why: "The first one goes out within 15 seconds of " +
        "the server starting. Check again in a moment." };
    }
    if (health.world.registration === "temporary") {
      return {
        state: "warn", title: title, summary: "Temporary name, updated " + timeText(directory.last_ok),
        why: "Your world has a temporary name, which can change. Register it to keep a name people can come back to.",
        steps: ["Connect your Darkride account and set up your world's ID under Settings > Metaverse."]
      };
    }
    return { state: "ok", title: title, summary: "Updated " + timeText(directory.last_ok) };
  }

  function checkAccess(health) {
    var access = health.access;
    var title = "Who can come in";
    var capacity = access.max_capacity > 0 ? ", up to " + access.max_capacity + " at a time" : "";
    if (access.anonymous) {
      return { state: "ok", title: title, summary: "Anyone" + capacity };
    }
    if (access.logged_in) {
      return { state: "ok", title: title, summary: "Anyone signed in to Darkride" + capacity };
    }
    if (access.friends) {
      return {
        state: "info", title: title, summary: "Only your friends" + capacity,
        why: "Other people can't come in, by your choice. Change it under Settings > Permissions (\"Come in\")."
      };
    }
    return {
      state: "fail", title: title, summary: "Nobody else",
      why: "No group of visitors is allowed to come in, so only this computer and people you've listed by name can.",
      steps: ["Under Settings > Permissions, give \"Come in\" to the people who should be able to visit."]
    };
  }

  function checkAdmin(settings) {
    var title = "Admin pages";
    var port = location.port || (location.protocol === "https:" ? "443" : "80");
    if (settings.http_password_set === true || settings.oauth_in_use === true) {
      return {
        state: "ok", title: title, summary: "Locked",
        why: "Keep their port (TCP " + port + ") closed to the internet anyway, and reach them through a tunnel or a " +
          "reverse proxy when you're away."
      };
    }
    return {
      state: "warn", title: title, summary: "Not locked",
      why: "Anyone who can reach these pages can change your world's settings, replace its content or restart it.",
      steps: [
        "Set up a sign-in under Settings > Admin sign-in.",
        "Never open TCP " + port + " to the internet on your router or firewall."
      ]
    };
  }

  function overallText(results) {
    var worst = _.max(results, function(result) { return STATES[result.state].order; });
    if (worst.state === "fail") {
      return { state: "fail", text: "Something stops visitors from coming in." };
    }
    if (worst.state === "warn") {
      return { state: "warn", text: "Visitors can probably come in, but a few things need a look." };
    }
    return { state: "ok", text: "Everything this server can check by itself is fine." };
  }

  function readLastPass() {
    try {
      return Number(localStorage.getItem(LAST_PASS_KEY) || 0);
    } catch (e) {
      return 0;
    }
  }

  function render(results, health) {
    var overall = overallText(results);
    if (overall.state === "ok") {
      try {
        localStorage.setItem(LAST_PASS_KEY, String(health.checked_at));
      } catch (e) {
      }
    }
    $('#dr-health-overall').text(overall.text).attr('class', 'dr-health-overall dr-health-' + overall.state);

    var $list = $('#dr-health-list').empty();
    _.each(results, function(result, index) {
      var $row = $('<li class="dr-health-row">').addClass('dr-health-' + result.state);
      var $head = $('<button type="button" class="dr-health-head">').attr('aria-expanded', 'false');
      $head.append($('<span class="dr-health-icon" aria-hidden="true">'));
      $head.append($('<span class="dr-health-title">').text(result.title));
      $head.append($('<span class="dr-health-summary">').text(result.summary));
      $head.append($('<span class="dr-health-state">').text(STATES[result.state].label));
      $row.append($head);
      if (result.why || result.steps) {
        var $more = $('<div class="dr-health-more">').hide();
        if (result.why) {
          $more.append($('<p>').text(result.why));
        }
        if (result.steps) {
          var $steps = $('<ol>');
          _.each(result.steps, function(step) {
            $steps.append($('<li>').text(step));
          });
          $more.append($('<p class="dr-health-fix">').text("How to fix it")).append($steps);
        }
        $row.append($more);
        // open by itself where something needs fixing
        if (result.state === "fail" || result.state === "warn") {
          $more.show();
          $head.attr('aria-expanded', 'true');
        }
      } else {
        $head.prop('disabled', true);
      }
      $list.append($row);
    });

    var lastPass = readLastPass();
    $('#dr-health-when').text("Checked " + new Date(health.checked_at).toLocaleTimeString() +
      " · Last time everything passed: " + (lastPass ? new Date(lastPass).toLocaleString() : "not yet"));

    var ip = publicAddress(health);
    if (ip) {
      var link = "hifi://" + ip + ":" + health.network.local_port;
      $('#dr-health-link').text(link).attr('href', link);
      $('#dr-health-link-row').show();
    } else {
      $('#dr-health-link-row').hide();
    }
  }

  // for asking for help: what was checked and found, never the sign-in or keys (the server leaves those out)
  function diagnosticsText(results, health, hideAddress) {
    var ICONS = { ok: "🟢", info: "🔵", unknown: "⚪", warn: "🟡", fail: "🔴" };
    var hide = function(text) {
      return hideAddress ? String(text).replace(/\b\d{1,3}(\.\d{1,3}){3}\b/g, "x.x.x.x") : String(text);
    };
    var lines = [];
    lines.push("**Darkride world check** · version " + health.version + " · " + new Date(health.checked_at).toISOString());
    _.each(results, function(result) {
      lines.push(ICONS[result.state] + " " + result.title + ": " + hide(result.summary));
    });
    lines.push("");
    lines.push("```");
    lines.push("world: " + health.world.registration + ", networking: " + health.network.automatic_networking +
      ", ICE server: " + (health.network.ice_server_set ? (health.network.connected_to_ice_server ? "connected" :
      "not connected") : "none"));
    lines.push("domain port: UDP " + health.network.local_port + ", local " + hide(health.network.local.ip) +
      ", public " + (health.network.public ? hide(health.network.public.ip) + ":" + health.network.public.port :
      "unknown") + ", STUN " + (health.network.stun_completed ? "done" : "not done"));
    _.each(health.services, function(service) {
      lines.push(service.type + ": " + (service.running ? "running, public " + hide(service.public.ip) + ":" +
        service.public.port : "waiting"));
    });
    lines.push("directory: " + health.directory.metaverse_url + ", heartbeats " +
      (health.directory.heartbeats_running ? "on" : "off") + ", last ok " + timeText(health.directory.last_ok) +
      (health.directory.last_failed ? ", last failed " + timeText(health.directory.last_failed) + " (" +
      health.directory.last_error + ")" : ""));
    lines.push("```");
    return lines.join("\n");
  }

  // the admin pages are often plain http on a home network, where the clipboard API isn't available, so the older
  // way is tried too
  function copyWithSelection(text) {
    var $area = $('<textarea readonly>').val(text).css({ position: 'fixed', top: '-1000px' }).appendTo('body');
    $area[0].select();
    var copied = false;
    try {
      copied = document.execCommand('copy');
    } catch (e) {
    }
    $area.remove();
    return copied;
  }

  function copyText(text) {
    var done = $.Deferred();
    var fallBack = function() {
      if (copyWithSelection(text)) {
        done.resolve();
      } else {
        done.reject();
      }
    };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function() { done.resolve(); }, fallBack);
    } else {
      fallBack();
    }
    return done.promise();
  }

  function runCheck() {
    var $button = $('#dr-health-run').prop('disabled', true).text("Checking…");
    $.when($.getJSON("/api/health"), $.getJSON("/settings.json")).done(function(healthReply, settingsReply) {
      var health = healthReply[0];
      var settings = settingsReply[0];
      lastHealth = health;
      lastResults = [
        checkServices(health),
        checkPublicAddress(health),
        checkOutside(health),
        checkServicePorts(health),
        checkDirectory(health),
        checkAccess(health),
        checkAdmin(settings)
      ];
      render(lastResults, health);
      $('#dr-health-copy').prop('disabled', false);
    }).fail(function() {
      $('#dr-health-overall').text("The check couldn't run: the server didn't answer. Refresh the page and try again.")
        .attr('class', 'dr-health-overall dr-health-fail');
    }).always(function() {
      $button.prop('disabled', false).text("Check again");
    });
  }

  $card.on('click', '.dr-health-head', function() {
    var $more = $(this).next('.dr-health-more');
    $more.slideToggle(150);
    $(this).attr('aria-expanded', $(this).attr('aria-expanded') === 'true' ? 'false' : 'true');
  });

  $('#dr-health-run').on('click', runCheck);

  $('#dr-health-copy').on('click', function() {
    if (!lastResults) {
      return;
    }
    var text = diagnosticsText(lastResults, lastHealth, $('#dr-health-hide-ip').is(':checked'));
    var $button = $(this);
    $('#dr-health-diagnostics').hide();
    copyText(text).done(function() {
      $button.text("Copied");
      setTimeout(function() { $button.text("Copy diagnostics"); }, 2000);
    }).fail(function() {
      // the browser wouldn't let the page copy: show it, selected, to copy by hand
      $('#dr-health-diagnostics').show().find('textarea').val(text).trigger('select');
    });
  });

  $('#dr-health-copy-link').on('click', function() {
    var $button = $(this);
    copyText($('#dr-health-link').attr('href')).done(function() {
      $button.text("Copied");
      setTimeout(function() { $button.text("Copy"); }, 2000);
    });
  });

  runCheck();
});
