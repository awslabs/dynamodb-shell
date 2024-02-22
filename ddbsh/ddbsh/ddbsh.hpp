// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * ddbsh.hpp
 *
 * The main class for ddbsh, instantiated from main.cpp
 *
 * Amrith. 2022-08-10
 *
 */

#ifndef __DDBSH_HPP_DEFINED__
#define __DDBSH_HPP_DEFINED__

#include <string>

#include <aws/core/Aws.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/core/config/AWSProfileConfig.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/AssumeRoleRequest.h>
#include <unistd.h>
#include "logging.hpp"
#include "input-source.hpp"

namespace ddbsh
{
    class CDDBSh
    {
    public:
        ~CDDBSh();
        CDDBSh();

        // return the singleton instance
        static CDDBSh * getInstance();

        void quit();
        void setRegionAndEndpoint(std::string region, std::string endpoint);
        std::string getRegion();
        Aws::DynamoDB::DynamoDBClient * getClient();

        // get unique prefix
        std::string st_unique_prefix();

        int runcommand(char * command) {
            setinputstring(command);
            int retval = this->run();
            return retval;
        };

        int runfile(char * filename) {
            this->abort_on_error(m_abort_on_error);
            return __runfile(filename);
        };

        int __runfile(char * filename) {
            FILE * infp = fopen(filename, "r");

            if (infp == (FILE *) NULL) {
                perror(filename);
                exit(1);
            }

            setinputfile(infp);

            int retval = this->run();
            fclose(infp);
            if (retval)
                exit(1);

            return 0;
        };

        int runstdin() {
            setinputstdin();
            return this->run();
        };

        // runinit loads the default configuration file
        int runinit() {
            if (access(m_config_file.c_str(), R_OK) == 0) {
                this->abort_on_error(true);
                return __runfile((char *) m_config_file.c_str());
            }

            return 0;
        };

        bool abort_on_error() {
            return m_abort_on_error;
        };

        void abort_on_error(bool s) {
            m_abort_on_error = s;
        };

    private:
        std::string m_history_file;
        std::string m_region;
        std::string m_endpoint;
        std::string m_config_file;

        bool m_quit;
        bool m_reconnect;

        Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
        Aws::SDKOptions m_options;

        int run();

        void reconnect();
        bool checkConnection();

        // some operations like the symboltable require a unique
        // identifier.
        int st_uniq;

        // command line abort on error
        bool m_abort_on_error;

	// In order to handle delegated access and the
	// AWS_USER_PROFILE get the credentials to use. The method
	// here is based on the one I found in
	// https://github.com/aws/aws-sdk-cpp/issues/150#issuecomment-416646025
	std::string getProfileName();
	Aws::Config::Profile getProfile();
	Aws::Auth::AWSCredentials getCredentials();
    };
};
#endif
