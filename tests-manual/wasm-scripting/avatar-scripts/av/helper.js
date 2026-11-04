// helper.js: included by probe.js (Script.include of a file next to the .fst). It runs in the avatar script's own
// engine, under the same rules; it reports what it got.
var probeHelper = {
    ran: true,
    menuRefused: (function () {
        try { var exists = Menu.menuExists("Edit"); return typeof exists !== "boolean"; } catch (e) { return true; }
    })()
};
