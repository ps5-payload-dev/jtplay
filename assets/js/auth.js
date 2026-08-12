// SPDX-License-Identifier: GPL-3.0-or-later
//
// Credentials, collected only when a provider says it needs them.
//
// The shell knows nothing about how a provider authenticates. browse() and
// resolve() are called with no credentials; if the provider finds that what
// is being opened is protected, it throws:
//
//   throw Auth.required({
//     realm:  "smb://192.168.1.10:445",     // what the password unlocks
//     title:  "Sign in to MyNAS",           // dialog heading
//     prompt: "The server asks for a username and password."
//   });
//
// The shell then supplies credentials for that realm - from storage if it
// has them, otherwise by asking - and calls again with {user, pass, realm}
// as the last argument. Throw again for the same realm and the saved copy is
// dropped and the dialog comes back with the reason showing.
//
// Realms rather than providers are the unit of authentication, so one plugin
// can hand out a public library, a share behind a password, and another
// behind a different password, each with its own login. Credentials arrive
// tagged with the realm they were issued for, so a plugin serving several
// servers can check `creds.realm` before putting a password on the wire; the
// shell never volunteers credentials for a realm nobody named.
//
// Storage is base64 of a JSON blob in localStorage, which keeps passwords
// out of plain sight but is obfuscation rather than encryption: treat console
// storage as readable by anyone holding the console.

(function() {
  "use strict";

  var STORE_PREFIX = "jtplay.auth.";

  var $ = function(id) { return document.getElementById(id); };

  // ------------------------------------------------------------------
  // The "please sign in" signal
  // ------------------------------------------------------------------

  // opts: {realm, title, prompt, user}
  function required(opts) {
    opts = opts || {};
    var err = new Error(opts.prompt || "Sign-in required");
    err.name = "AuthRequired";
    err.auth = true;
    err.realm = opts.realm || "";
    err.title = opts.title || "";
    err.prompt = opts.prompt || "";
    err.user = opts.user || "";
    return err;
  }

  // The realm is what makes the signal actionable, so one without it is just
  // an error like any other and is reported rather than acted on.
  function isRequired(err) {
    return !!(err && err.auth && err.realm);
  }

  // The two answers that mean "not with those credentials". 401 is the
  // conventional challenge; 403 is what a server sends when it would rather
  // not advertise that a login exists. jtplay's smb proxy uses both.
  function refused(res) {
    return !!res && (res.status === 401 || res.status === 403);
  }

  // ------------------------------------------------------------------
  // Credential store, keyed by realm
  // ------------------------------------------------------------------

  var memory = {};

  // btoa() only takes bytes, so round-trip through UTF-8 first; passwords
  // are not always ASCII.
  function b64encode(str) {
    return window.btoa(window.unescape(encodeURIComponent(str)));
  }

  function b64decode(str) {
    return decodeURIComponent(window.escape(window.atob(str)));
  }

  function keyFor(realm) {
    return STORE_PREFIX + realm;
  }

  function readRecord(realm) {
    if (memory[realm]) {
      return memory[realm];
    }
    try {
      var raw = window.localStorage.getItem(keyFor(realm));
      if (raw) {
        var rec = JSON.parse(b64decode(raw));
        if (rec && typeof rec.user === "string") {
          memory[realm] = rec;
          return rec;
        }
      }
    } catch (e) {
      // Storage can be off, full, or holding something we no longer parse.
      // Either way there is nothing saved for this realm.
    }
    return null;
  }

  // Credentials for a realm, or null. Tagged with the realm so a provider can
  // tell what it has been handed.
  function stored(realm) {
    var rec = readRecord(realm);
    return rec ? {user: rec.user, pass: rec.pass || "", realm: realm} : null;
  }

  // `owner` is the provider key, kept only so the sources screen can say who
  // is signed in to what and offer to sign out again.
  function save(realm, creds, meta, persist) {
    meta = meta || {};
    var rec = {
      user: creds.user,
      pass: creds.pass,
      owner: meta.owner || "",
      label: meta.label || ""
    };
    memory[realm] = rec;
    try {
      if (persist) {
        window.localStorage.setItem(keyFor(realm),
          b64encode(JSON.stringify(rec)));
      } else {
        window.localStorage.removeItem(keyFor(realm));
      }
    } catch (e) {
      // The session copy still works without storage.
    }
  }

  function forget(realm) {
    delete memory[realm];
    try {
      window.localStorage.removeItem(keyFor(realm));
    } catch (e) {
      // Nothing to clean up.
    }
  }

  function labelFor(realm) {
    var rec = readRecord(realm);
    return (rec && rec.label) || "";
  }

  // Every realm we hold credentials for on behalf of a provider. Providers
  // are rediscovered on every refresh, so they are matched by their stable
  // key rather than by object identity.
  function accounts(provider) {
    var owner = provider && provider.key;
    var found = [];
    var seen = {};
    var realm;

    if (!owner) {
      return found;
    }

    for (realm in memory) {
      if (Object.prototype.hasOwnProperty.call(memory, realm) &&
          memory[realm].owner === owner) {
        seen[realm] = true;
        found.push({realm: realm, user: memory[realm].user});
      }
    }

    try {
      for (var i = 0; i < window.localStorage.length; i++) {
        var key = window.localStorage.key(i);
        if (!key || key.indexOf(STORE_PREFIX) !== 0) {
          continue;
        }
        realm = key.substring(STORE_PREFIX.length);
        if (seen[realm]) {
          continue;
        }
        var rec = readRecord(realm);
        if (rec && rec.owner === owner) {
          found.push({realm: realm, user: rec.user});
        }
      }
    } catch (e) {
      // Without storage the in-memory list is all there is.
    }

    return found;
  }

  // Drops every login a provider holds. Providers usually cache something
  // derived from the credentials - a url, a token, a handle - so each is told
  // which realm went, or signing out would leave a live session behind and
  // the next call would quietly succeed.
  function signOut(provider) {
    accounts(provider).forEach(function(account) {
      forget(account.realm);
      notifySignOut(provider, account.realm);
    });
  }

  function notifySignOut(provider, realm) {
    if (provider && typeof provider.signOut === "function") {
      try {
        provider.signOut(realm);
      } catch (e) {
        // A provider that cannot tidy up is not a reason to keep the
        // credentials around.
        console.error(e);
      }
    }
  }

  // ------------------------------------------------------------------
  // Dialog
  // ------------------------------------------------------------------

  // Navigation order. Each name maps to an #auth-row-<name> element.
  var ROWS = ["user", "pass", "remember", "ok", "cancel"];

  var dlg = null; // {resolve, sel, editing, remember}

  function paint() {
    ROWS.forEach(function(name, i) {
      var el = $("auth-row-" + name);
      if (!el) {
        return;
      }
      el.className = el.className.replace(/ ?(selected|editing)/g, "") +
        (i === dlg.sel ? " selected" : "") +
        (i === dlg.sel && dlg.editing ? " editing" : "");
    });
    $("auth-remember-box").className =
      "auth-box" + (dlg.remember ? " on" : "");
    // X opens the keyboard on a text field and acts everywhere else, so the
    // hint says which one it is rather than going blank.
    $("auth-hint-x").textContent = dlg.sel < 2 ? "\u00a0Edit" : "\u00a0Select";
  }

  function setError(msg) {
    var el = $("auth-error");
    el.textContent = msg || "";
    el.className = "auth-error" + (msg ? " on" : "");
  }

  function open(opts) {
    opts = opts || {};
    return new Promise(function(resolve) {
      dlg = {
        resolve: resolve,
        // Coming back after a rejected password, the username is usually
        // right, so start on the field that needs the correction.
        sel: opts.user ? 1 : 0,
        editing: false,
        remember: opts.remember !== false
      };

      $("auth-title").textContent = opts.title || "Sign in";
      $("auth-sub").textContent = opts.message || "";
      $("auth-user").value = opts.user || "";
      $("auth-pass").value = "";
      setError(opts.error || "");

      $("auth-overlay").className = "on";
      paint();
    });
  }

  function close(result) {
    var resolve = dlg.resolve;
    stopEditing();
    $("auth-overlay").className = "";
    $("auth-pass").value = "";
    dlg = null;
    resolve(result);
  }

  function startEditing() {
    var input = dlg.sel === 0 ? $("auth-user") : $("auth-pass");
    dlg.editing = true;
    paint();
    input.focus();
    // Park the caret at the end so typing appends rather than overwrites.
    var len = input.value.length;
    if (input.setSelectionRange) {
      try {
        input.setSelectionRange(len, len);
      } catch (e) {
        // Not every input type supports selection ranges.
      }
    }
  }

  function stopEditing() {
    if (dlg && dlg.editing) {
      dlg.editing = false;
      paint();
    }
    var active = document.activeElement;
    if (active && active.blur && /^auth-(user|pass)$/.test(active.id)) {
      active.blur();
    }
  }

  function move(delta) {
    dlg.sel = (dlg.sel + ROWS.length + delta) % ROWS.length;
    paint();
  }

  function submit() {
    // A blank password is normal on a share that only wants a username, so
    // only the username is insisted on.
    var user = $("auth-user").value.trim();
    if (!user) {
      setError("Enter a username.");
      dlg.sel = 0;
      paint();
      return;
    }
    close({user: user, pass: $("auth-pass").value, remember: dlg.remember});
  }

  function activate() {
    switch (ROWS[dlg.sel]) {
    case "user":
    case "pass":
      startEditing();
      break;
    case "remember":
      dlg.remember = !dlg.remember;
      paint();
      break;
    case "ok":
      submit();
      break;
    case "cancel":
      close(null);
      break;
    }
  }

  function printable(e) {
    return e.key && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey;
  }

  // Capture phase: while the dialog is up it owns the controller, so the
  // browse/watch handlers in app.js never see these keys.
  document.addEventListener("keydown", function(e) {
    if (!dlg) {
      return;
    }
    e.stopPropagation();

    var code = e.keyCode;

    if (dlg.editing) {
      // X commits the field and steps on, O leaves it as it was; every other
      // key belongs to the text field.
      if (code === 13) {
        e.preventDefault();
        stopEditing();
        move(+1);
      } else if (code === 27) {
        e.preventDefault();
        stopEditing();
      }
      return;
    }

    switch (code) {
    case 38: // up
      e.preventDefault();
      move(-1);
      break;
    case 40: // down
      e.preventDefault();
      move(+1);
      break;
    case 37: // left
      e.preventDefault();
      if (ROWS[dlg.sel] === "cancel") {
        move(-1);
      }
      break;
    case 39: // right
      e.preventDefault();
      if (ROWS[dlg.sel] === "ok") {
        move(+1);
      }
      break;
    case 13: // X
      e.preventDefault();
      activate();
      break;
    case 27: // O
      e.preventDefault();
      close(null);
      break;
    default:
      // Typing straight at a highlighted text field starts editing and keeps
      // the keystroke, which is what a usb keyboard user expects.
      if (printable(e) && dlg.sel < 2) {
        startEditing();
      }
    }
  }, true);

  // The on-screen keyboard can dismiss itself without sending a key event, so
  // keep our idea of "editing" in step with the real focus.
  ["auth-user", "auth-pass"].forEach(function(id) {
    var el = $(id);
    if (el) {
      el.addEventListener("blur", function() {
        if (dlg && dlg.editing) {
          dlg.editing = false;
          paint();
        }
      });
    }
  });

  // ------------------------------------------------------------------
  // Asking
  // ------------------------------------------------------------------

  // Puts the dialog up for the realm named in `err` and files whatever comes
  // back under that realm. `rejected` adds the "those were refused" banner.
  // Resolves to {user, pass, realm}, or null if the viewer backs out.
  function ask(provider, err, rejected) {
    var realm = err.realm;
    var known = stored(realm);
    var title = err.title || labelFor(realm) || (provider && provider.name) ||
      "Sign in";

    return open({
      title: title,
      message: err.prompt || "This needs a username and password.",
      error: rejected ? "That username and password were not accepted." : "",
      user: err.user || (known && known.user) || ""
    }).then(function(res) {
      if (!res) {
        return null;
      }
      save(realm, res,
           {owner: provider && provider.key, label: title}, res.remember);
      return {user: res.user, pass: res.pass, realm: realm};
    });
  }

  window.Auth = {
    required: required,
    isRequired: isRequired,
    refused: refused,

    stored: stored,
    forget: forget,
    accounts: accounts,
    signOut: signOut,
    notifySignOut: notifySignOut,
    labelFor: labelFor,

    ask: ask,
    prompt: open,
    isOpen: function() { return !!dlg; }
  };
})();
