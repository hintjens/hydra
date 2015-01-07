;;  =========================================================================
;;    hydra_server
;;
;;    ** WARNING *************************************************************
;;    THIS SOURCE FILE IS 100% GENERATED. If you edit this file, you will lose
;;    your changes at the next build cycle. This is great for temporary printf
;;    statements. DO NOT MAKE ANY CHANGES YOU WISH TO KEEP. The correct places
;;    for commits are:
;;
;;     * The XML model used for this code generation: hydra_server.xml, or
;;     * The code generation script that built this file: zproto_server_clj
;;    ************************************************************************
;;    Copyright (c) the Contributors as noted in the AUTHORS file.       
;;    This file is part of zbroker, the ZeroMQ broker project.           
;;                                                                       
;;    This Source Code Form is subject to the terms of the Mozilla Public
;;    License, v. 2.0. If a copy of the MPL was not distributed with this
;;    file, You can obtain one at http://mozilla.org/MPL/2.0/.           
;;    =========================================================================

(ns org.zproto.hydra-server
  (:refer-clojure :exclude [send])
  (:require [zeromq.zmq :as zmq]
            [org.zproto.hydra-proto :as msg])
  (:import  [org.zproto HydraProto]))

;;
;; The HydraServerBackend Protocol specifies
;; the actions that have to be implemented for a functional server.
;; Currently, return values are expected to match be the arguments to
;; the returning send function as a vector.
;;
(defprotocol HydraServerBackend
  (set-server-identity [this msg identity nickname])
  (calculate-status-for-client [this msg oldest newest])
  (signal-command-invalid [this msg]))

(defn terminate [{:keys [state]} routing-id _]
  (swap! state dissoc routing-id))

(defn next-state [to-state]
  (fn [{:keys [state]} routing-id _]
    (swap! state assoc-in [routing-id] to-state)))

;;
;; Wrapper function that facilitates correct parameter
;; assignments to the send-fn depending on the shape
;; of the response generated by the backend
;;
(defn send [message-id]
   (fn [{:keys [socket]} _ ^HydraProto msg]
      (msg/id! msg message-id)
      (.send msg (:socket socket))))

;;
;; Creates a function that calls the backend action-fn with the
;; appropriate parameters extracted from the msg.
;;
(defmacro action [action-fn & extractors]
  `(fn [{:keys [~'backend]} ~'_ ~'msg]
     (~action-fn ~'backend ~'msg ~@(mapv (fn [e] (list e 'msg)) extractors))
     ~'msg))

;;
;; Encodes the transition from states via events through
;; a number of actions. This is pretty close to the structure
;; given in the model, super states are expanded.
;;
(def state-events {
  :start {
    HydraProto/HELLO [ (action set-server-identity .identity .nickname) (send HydraProto/HELLO_OK) (next-state :connected) ]
    HydraProto/EXPIRED [ terminate ]
    HydraProto/EXCEPTION [ (send HydraProto/ERROR) terminate ]
    :* [ (action signal-command-invalid) (send HydraProto/ERROR) terminate ]
  }
  :connected {
    HydraProto/STATUS [ (action calculate-status-for-client .oldest .newest) (send HydraProto/STATUS_OK) ]
    HydraProto/HEADER [ (send HydraProto/HEADER_OK) ]
    HydraProto/FETCH [ (send HydraProto/FETCH_OK) ]
    HydraProto/GOODBYE [ (send HydraProto/GOODBYE_OK) terminate ]
    HydraProto/EXPIRED [ terminate ]
    HydraProto/EXCEPTION [ (send HydraProto/ERROR) terminate ]
    :* [ (action signal-command-invalid) (send HydraProto/ERROR) terminate ]
  }
  :defaults {
    HydraProto/EXPIRED [ terminate ]
    HydraProto/EXCEPTION [ (send HydraProto/ERROR) terminate ]
    :* [ (action signal-command-invalid) (send HydraProto/ERROR) terminate ]
  }})

(def determine-actions
  (memoize
   (fn [state event-id]
     (or (get-in state-events [state event-id])
         (get-in state-events [state :*])))))

(defn maybe-setup-session [state routing-id]
  (if (get state routing-id)
    state
    (assoc state routing-id :start)))

(defn match-msg
  [{:keys [state] :as server} ^HydraMsg msg]
  (let [id (.id msg)
        routing-id (.getData (.routingId msg))
        initialized-state (-> (swap! state maybe-setup-session routing-id)
                              (get routing-id))]
    (reduce (fn [msg handler]
               (handler server routing-id msg))
            msg
            (determine-actions initialized-state id))))

(defrecord Server [socket state backend])

(defn server-loop
  [socket backend]
  (let [server (Server. socket (atom {}) backend)]
    (loop []
      (when-let [received (msg/recv socket)]
        (try
          (match-msg server received)
          (catch Exception e
            (.printStackTrace e))))
      (if (not (.isInterrupted (Thread/currentThread)))
        (recur)
        (println "Server shutdown")))))
