// ==UserScript==
// @name         GeoFS Controle - Feedback haptico
// @namespace    insper-embarcados-aps2
// @version      1.0
// @description  Le o estado do GeoFS (crash/stall) e avisa a ponte PC, que
//               repassa ao controle como vibracao/buzzer (ponto extra:
//               "feedback do jogo").
// @match        https://www.geo-fs.com/*
// @grant        none
// ==/UserScript==

/*
 * Instalacao:
 *   1. Instale a extensao Tampermonkey (ou Violentmonkey) no navegador.
 *   2. Crie um novo script e cole este arquivo.
 *   3. Rode a ponte PC (main.py) - ela sobe o servidor em 127.0.0.1:8765.
 *   4. Abra o GeoFS: ao bater (crash) ou estolar (stall), o controle vibra.
 */

(function () {
    "use strict";

    const ENDPOINT = "http://127.0.0.1:8765/event";
    var lastCrash = false;
    var lastStall = false;

    function notify(type) {
        // fire-and-forget; ignora erros (ponte pode estar fechada)
        try {
            fetch(ENDPOINT + "?type=" + type, { mode: "no-cors" });
        } catch (e) {
            /* nada */
        }
    }

    setInterval(function () {
        try {
            const ac = window.geofs && geofs.aircraft && geofs.aircraft.instance;
            if (!ac) {
                return;
            }

            // --- crash ---
            const crashed = !!ac.crashed;
            if (crashed && !lastCrash) {
                notify("crash");
            }
            lastCrash = crashed;

            // --- stall (heuristica; depende da aeronave) ---
            // GeoFS expoe avisos em animationValue; caimos para AoA alto em voo.
            var stall = false;
            if (ac.animationValue && ac.animationValue.stallWarning) {
                stall = true;
            } else if (typeof ac.aoa === "number" && !ac.groundContact) {
                stall = ac.aoa > 16; // graus, aproximado
            }
            if (stall && !lastStall) {
                notify("stall");
            }
            lastStall = stall;
        } catch (e) {
            /* nada */
        }
    }, 250);
})();
