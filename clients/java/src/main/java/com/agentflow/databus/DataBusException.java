package com.agentflow.databus;

/**
 * Exception thrown for DataBus operations.
 *
 * <p>Wraps JSON-RPC errors and communication failures.
 */
public class DataBusException extends Exception {

    private final int code;

    /**
     * Create an exception with a message.
     *
     * @param message Error message
     */
    public DataBusException(String message) {
        super(message);
        this.code = -1;
    }

    /**
     * Create an exception with a message and cause.
     *
     * @param message Error message
     * @param cause   Underlying cause
     */
    public DataBusException(String message, Throwable cause) {
        super(message, cause);
        this.code = -1;
    }

    /**
     * Create an exception with a JSON-RPC error code and message.
     *
     * @param code    JSON-RPC error code
     * @param message Error message
     */
    public DataBusException(int code, String message) {
        super(message);
        this.code = code;
    }

    /**
     * Get the JSON-RPC error code.
     *
     * @return Error code, or -1 if not a JSON-RPC error
     */
    public int getCode() {
        return code;
    }

    @Override
    public String toString() {
        if (code != -1) {
            return "DataBusException{code=" + code + ", message='" + getMessage() + "'}";
        }
        return "DataBusException: " + getMessage();
    }
}